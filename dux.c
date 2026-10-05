/* dux.c - 8-bit stack machine, CPU core
 *
 * Fetch-decode-execute over a 64KB address space with two 256-byte circular
 * stacks. No dynamic allocation, no libc calls beyond nothing at all.
 *
 * The mode-bit trick: 32 base opcodes combined with three flags (short,
 * return, keep) give 256 distinct operations, all encoded in a single byte.
 * Each base opcode is written once as a macro that expands to its eight cases.
 */

#include "dux.h"
#include <stdio.h>
#include <stdlib.h>

/* --------------------------------------------------------------- the machine */

Dux dux;

/* Host device callbacks. NULL means the device page is plain memory. */
static void  *dev_ctx = 0;
static DeiFn  dei_fn   = 0;
static DeoFn  deo_fn   = 0;

void dux_attach(void *ctx, DeiFn dei, DeoFn deo) {
	dev_ctx = ctx;
	dei_fn  = dei;
	deo_fn  = deo;
}

/* ------------------------------------------------------------------ memory */

Uint8 dux_load(Uint16 addr) {
	if(dei_fn && addr >= IO_BASE && addr < IO_BASE + IO_SIZE)
		return dei_fn(dev_ctx, (Uint8)(addr & 0xff));
	return dux.mem[addr];
}

void dux_store(Uint16 addr, Uint8 val) {
	/* Shadow every write into memory so that ports no device claims still
	   read back what was written, matching the "unhandled port acts like a
	   normal memory cell" rule. */
	dux.mem[addr] = val;
	if(deo_fn && addr >= IO_BASE && addr < IO_BASE + IO_SIZE)
		deo_fn(dev_ctx, (Uint8)(addr & 0xff), val);
}

/* ------------------------------------------------------------- stack access
 *
 * Both pointers are Uint8, so increment and decrement wrap on their own.
 * That is the entire underflow and overflow policy: silent circular wrap.
 *
 * The pointer always addresses one past the top element.
 */

#define DECW       dux.wst[--dux.wstp]
#define DECR       dux.rst[--dux.rstp]

void dux_push(Uint8 val) {
	dux.wst[dux.wstp++] = val;
}

Uint8 dux_pop(void) {
	return dux.wst[--dux.wstp];
}

/* ------------------------------------------------------------------ devices */

void dux_dei(Uint8 addr) {
	Uint8 val = dux_load((Uint16)(IO_BASE + (Uint16)addr));
	dux.wst[dux.wstp++] = val;
}

void dux_deo(Uint8 addr) {
	Uint8 val = dux.wst[--dux.wstp];
	dux_store((Uint16)(IO_BASE + (Uint16)addr), val);
}

/* -------------------------------------------------------------- microcode
 *
 * Within an opcode body, _2 is 1 in short mode and 0 otherwise, _r is 1 in
 * return mode and 0 otherwise. POx/PUx therefore read and write the right
 * stack at the right width without the opcode having to know.
 */

#define PO1(o)   { o = _r ? DECR : DECW; }
#define PO2(o)   { if(_r) { o = DECR; o |= DECR << 8; } \
                   else     { o = DECW; o |= DECW << 8; } }
#define POx(o)   { if(_2) PO2(o) else PO1(o) }

#define PU1(i)   { if(_r) dux.rst[dux.rstp++] = i; else dux.wst[dux.wstp++] = i; }
#define RP1(i)   { if(_r) dux.wst[dux.wstp++] = i; else dux.rst[dux.rstp++] = i; }
#define PUx(i)   { if(_2) { PU1(((i) >> 8) & 0xff) PU1((i) & 0xff) } else PU1(i) }

/* Release without reading: pops 1 element, or 2 in short mode. */
#define REM      { if(_r) dux.rstp -= 1 + _2; else dux.wstp -= 1 + _2; }

/* Pop a whole item into a 2-slot array, low byte first. */
#define GET(o)   { if(_2) PO1(o[1]) PO1(o[0]) }
#define PUT(i)   { PU1(i[0]) if(_2) PU1(i[1]) }

/* Byte mode jumps relative to PC, which already points past the opcode.
   Short mode jumps absolute. */
#define JMPTO(x) { pc = _2 ? (Uint16)(x) : (Uint16)(pc + (Sint8)(x)); }

/* Device access. The port index is always 8-bit, but the resulting address
   must stay 16-bit: IO_BASE + 0x2e is 0x012e, and truncating it to a byte
   would land outside the device page. */
/* In short mode a device pair is two consecutive ports, and the HIGH byte is
   at the base: DEO_BODY stores y[0], which GET fills with the high half, at
   port base, and DEI_BODY pushes the base first, which is the high half too.
   So reads and writes agree. Note this is the opposite of the stack's own
   low-byte-on-top order and of memory, which is worth stating plainly. */
#define DEI_BODY(a) { Uint16 _p = (Uint16)(IO_BASE + (a)); \
                      if(_2) { PU1(dux_load(_p)); \
                               PU1(dux_load((Uint16)(_p + 1))); } \
                      else   { PU1(dux_load(_p)); } }

#define DEO_BODY(a) { Uint16 _p = (Uint16)(IO_BASE + (a)); \
                      dux_store(_p, y[0]); \
                      if(_2) dux_store((Uint16)(_p + 1), y[1]); }

/* Device ops read their port as a byte but always transfer a whole short, so
   the operand width follows the mode while the port stays 8-bit. This is the
   same shape as LDZ: an 8-bit index selecting a wider access. */
#define DEI_PORT(a) { Uint16 _p = (Uint16)(IO_BASE + (a)); \
                      if(_2) { PU1(dux_load(_p)); \
                               PU1(dux_load((Uint16)(_p + 1))); } \
                      else   { PU1(dux_load(_p)); } }

/* Zero-page access is always 8-bit indexed and wraps at 256. */
#define PEKZP(i,o)  { o[0] = dux.mem[i]; if(_2) o[1] = dux.mem[((i) + 1) & 0xff]; }
#define POKZP(i,j)  { dux.mem[i] = j[0]; if(_2) dux.mem[((i) + 1) & 0xff] = j[1]; }

/* Main-memory access routes through dux_load/dux_store so that writes into the
   device page take effect. */
#define PEK(i,o)  { o[0] = dux_load(i); if(_2) o[1] = dux_load((Uint16)((i) + 1)); }
#define POK(i,j)  { dux_store(i, j[0]); if(_2) dux_store((Uint16)((i) + 1), j[1]); }

/* Eight cases per opcode. Keep mode saves the pointer, runs the operand pops,
   then restores it, so the operands stay readable on the stack while their
   values are already captured in locals. */
#define OPC(opc, init, body) \
	case opc:                        { const int _2=0,_r=0; init ; body } break; \
	case opc|MODE_SHORT:             { const int _2=1,_r=0; init ; body } break; \
	case opc|MODE_RETURN:            { const int _2=0,_r=1; init ; body } break; \
	case opc|MODE_SHORT|MODE_RETURN: { const int _2=1,_r=1; init ; body } break; \
	case opc|MODE_KEEP:              { const int _2=0,_r=0; k=dux.wstp; init ; dux.wstp=k; body } break; \
	case opc|MODE_SHORT|MODE_KEEP:   { const int _2=1,_r=0; k=dux.wstp; init ; dux.wstp=k; body } break; \
	case opc|MODE_RETURN|MODE_KEEP:  { const int _2=0,_r=1; k=dux.rstp; init ; dux.rstp=k; body } break; \
	case opc|MODE_SHORT|MODE_RETURN|MODE_KEEP: \
	                                  { const int _2=1,_r=1; k=dux.rstp; init ; dux.rstp=k; body } break;

/* No-op body, for opcodes that only release or rearrange. */
#define NOOP        (void)0;

/* ----------------------------------------------------------------- eval */

/* Per-call-site stack accounting for DUX_TRACE_DEPTH. See the call site in
   dux_eval. The two arrays are indexed by return address: entered_at is the
   working-stack depth when the call that returns there was made. */
/* No per-call stack accounting here, and the reason is worth recording because
   two attempts at it failed before this one did.

   The idea was to watch each JSR2 record the depth it was entered at and each
   JMP2r2 compare against it, which would name the exact call site that does not
   hand back what it took - something no static pass can do, because the pass
   counts a call site once and a loop body is a site.

   Comparing against the depth at the call is simply the wrong measurement. A
   caller pushes its arguments before the target literal, so a routine that
   consumes exactly what it was handed returns far below where it started:
   @put16 is given a value and a port and correctly hands back nothing, and
   that reads as a five-byte leak. Every well-behaved routine looks broken.

   Comparing each return against the *previous* return at the same site is no
   better. That measures the caller's argument discipline as much as the
   callee's: @abs2 nets exactly zero on both its paths and still drifts, because
   its callers reach it with different amounts on the stack from one pass of a
   loop to the next. On cube it reported 921 spurious drifts and buried the real
   one.

   What survives is the check that already exists in the host: the vector must
   end balanced. It is coarser - it cannot name a site - but it is a fact about
   the whole run rather than an inference about one call, so it cannot be wrong
   in either of these ways. Localising the leak needs the static path sum in
   tools/routinegraph.py, which reports the set of nets a routine can reach
   instead of one number. */

int dux_eval(Uint16 pc, unsigned long steps) {
	unsigned long n;
	Uint32 a, b;
	Uint8 k;
	Uint8 x[2], y[2], z[2];

	for(n = 0; !steps || n < steps; n++) {
		Uint8 val = dux.mem[pc++];

		/* Publish the PC as we go, so a caller stepping one instruction at a
		   time can follow control flow instead of having to guess where a
		   jump went. Written back on every exit below. */

		switch(val) {

		/* ---- immediate family: opcode 0x00 with mode bits set ---- */

		case OP_LIT:
			dux.wst[dux.wstp++] = dux.mem[pc++];
			break;

		case OP_LIT2:
			dux.wst[dux.wstp++] = dux.mem[pc++];
			dux.wst[dux.wstp++] = dux.mem[pc++];
			break;

		case OP_LITR:
			dux.rst[dux.rstp++] = dux.mem[pc++];
			break;

		case OP_LIT2R:
			dux.rst[dux.rstp++] = dux.mem[pc++];
			dux.rst[dux.rstp++] = dux.mem[pc++];
			break;

		/* Absolute-ish 16-bit signed jump, relative to the byte after the
		   operand. */
		case OP_JMI: {
			Uint16 off;
			off = (Uint16)(dux.mem[pc] << 8 | dux.mem[pc + 1]);
			pc = (Uint16)(pc + 2 + (Sint16)off);
			break;
		}

		case OP_JCI: {
			Uint8 cond = DECW;
			if(cond) {
				Uint16 off;
				off = (Uint16)(dux.mem[pc] << 8 | dux.mem[pc + 1]);
				pc = (Uint16)(pc + 2 + (Sint16)off);
			} else {
				pc += 2;
			}
			break;
		}

		case OP_JSI: {
			Uint16 off;
			Uint16 ret = (Uint16)(pc + 2);
			dux.rst[dux.rstp++] = (Uint8)(ret >> 8);
			dux.rst[dux.rstp++] = (Uint8)(ret & 0xff);
			off = (Uint16)(dux.mem[pc] << 8 | dux.mem[pc + 1]);
			pc = (Uint16)(pc + 2 + (Sint16)off);
			break;
		}

		/* ---- stack shuffling ---- */

		OPC(INC, POx(a), PUx(a + 1))
		OPC(POP, REM, NOOP)
		OPC(NIP, GET(x) REM, PUT(x))
		OPC(SWP, GET(x) GET(y), PUT(x) PUT(y))
		OPC(ROT, GET(x) GET(y) GET(z), PUT(y) PUT(x) PUT(z))
		OPC(DUP, GET(x), PUT(x) PUT(x))
		OPC(OVR, GET(x) GET(y), PUT(y) PUT(x) PUT(y))

		/* ---- comparison: pushes 1 or 0, no flags register ---- */

		OPC(EQU, POx(a) POx(b), PU1(b == a))
		OPC(NEQ, POx(a) POx(b), PU1(b != a))
		OPC(GTH, POx(a) POx(b), PU1(b > a))
		OPC(LTH, POx(a) POx(b), PU1(b < a))

		/* ---- control flow ---- */

		/* DUX_TRACE_DEPTH reports, per call site, how much the working stack
		   grew while that call was outstanding. JSR2 records the depth it was
		   entered at, keyed by the return address it just stashed; the
		   matching JMP2r2 pops that address and compares. Anything non-zero is
		   a routine that does not hand back what it took, named by the address
		   of the instruction after the call.

		   This is what duxasm's stack pass cannot make. It is per *call*, not
		   per site, so a routine called from inside a loop reports its real
		   cost, and two call sites in one routine are told apart. Off unless
		   the variable is set. */
		/* One case covers JMP, JMP2, JMPr and JMP2r2: POx reads the target off the
		   stack the mode names, so a return-mode jump unstows its address
		   here and jumps to it. */
		OPC(JMP, POx(a), JMPTO(a))
		OPC(JCN, POx(a) PO1(b), if(b) JMPTO(a))
		/* JSR stashes the return address high byte first, then jumps.
		   In return mode the stacks swap, so it lands on the working stack. */
		OPC(JSR, POx(a), RP1(pc >> 8) RP1(pc & 0xff) JMPTO(a))
		OPC(STH, GET(x), RP1(x[0]) if(_2) RP1(x[1]))

		/* ---- memory ----
		   LDZ/STZ take an 8-bit zero-page index. LDR/STR take an 8-bit
		   signed offset from PC. LDA/STA take a 16-bit absolute address. */

		OPC(LDZ, PO1(a), PEKZP(a, x) PUT(x))
		OPC(STZ, PO1(a) GET(y), POKZP(a, y))
		OPC(LDR, PO1(a), PEK((Uint16)(pc + (Sint8)a), x) PUT(x))
		OPC(STR, PO1(a) GET(y), POK((Uint16)(pc + (Sint8)a), y))
		OPC(LDA, PO2(a), PEK((Uint16)a, x) PUT(x))
		OPC(STA, PO2(a) GET(y), POK((Uint16)a, y))

		/* ---- devices: the port index is always 8-bit ---- */

		OPC(DEI, PO1(a), DEI_BODY(a))
		OPC(DEO, PO1(a) GET(y), DEO_BODY(a))

		/* ---- arithmetic and logic ---- */

		OPC(ADD, POx(a) POx(b), PUx(b + a))
		OPC(SUB, POx(a) POx(b), PUx(b - a))
		OPC(MUL, POx(a) POx(b), PUx(b * a))
		/* Division by zero pushes zero rather than trapping. */
		OPC(DIV, POx(a) POx(b), PUx(a ? b / a : 0))
		OPC(AND, POx(a) POx(b), PUx(b & a))
		OPC(ORA, POx(a) POx(b), PUx(b | a))
		OPC(EOR, POx(a) POx(b), PUx(b ^ a))
		/* Control byte: high nibble shifts left, low nibble shifts right.
		   Right happens first. */
		OPC(SFT, PO1(a) POx(b), PUx(b >> (a & 0xf) << (a >> 4)))

		/* ---- break ---- */

		case BRK:
			dux.pc = pc;
			return 0;
		}
		dux.pc = pc;
	}
	return steps ? 1 : 0;
}

/* ----------------------------------------------------------------- control */

void dux_reset(void) {
	unsigned i;
	for(i = 0; i < MEM_SIZE; i++)
		dux.mem[i] = 0;
	dux.wstp = 0;
	dux.rstp = 0;
	dux.pc   = RESET_VECTOR;
}

void dux_soft_reset(void) {
	unsigned i;
	for(i = 0; i < ZP_SIZE; i++)
		dux.mem[i] = 0;
	dux.wstp = 0;
	dux.rstp = 0;
	dux.pc   = RESET_VECTOR;
}

/* ------------------------------------------------------------------- video */

void dux_fb_clear(Uint8 index) {
	unsigned i;
	for(i = 0; i < FB_BYTES; i++)
		dux.mem[FB_BASE + i] = index;
}

void dux_fb_pixel(int x, int y, Uint8 index) {
	Uint16 addr;
	Uint8  shift;
	if(x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
	addr  = (Uint16)(FB_BASE + (y * FB_WIDTH + x) / 4);
	shift = (Uint8)((x & 3) * 2);
	dux.mem[addr] = (Uint8)((dux.mem[addr] & ~(0x3 << shift)) | ((index & 0x3) << shift));
}

Uint8 dux_fb_get(int x, int y) {
	Uint16 addr;
	Uint8  shift;
	if(x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return 0;
	addr  = (Uint16)(FB_BASE + (y * FB_WIDTH + x) / 4);
	shift = (Uint8)((x & 3) * 2);
	return (Uint8)((dux.mem[addr] >> shift) & 0x3);
}