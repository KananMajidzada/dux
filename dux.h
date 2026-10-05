/* dux.h - 8-bit stack machine, CPU core
 *
 * An 8-bit stack machine with 64KB of addressable memory, a 256-byte working
 * stack and a 256-byte return stack. No general-purpose registers.
 *
 * The core has no dependencies beyond the C89 standard library and performs
 * no dynamic allocation. All state lives in a single Dux struct that the caller
 * owns, so multiple machines can coexist and the core can be linked into
 * freestanding targets.
 *
 * See ARCHITECTURE.md for the full specification.
 */

#ifndef DUX_H
#define DUX_H

/* ------------------------------------------------------------------ types */

typedef unsigned char Uint8;
typedef signed char Sint8;
typedef unsigned short Uint16;
typedef unsigned int Uint32;
typedef signed short Sint16;

/* ------------------------------------------------------------ memory map */

#define MEM_SIZE       65536

#define ZP_BASE        0x0000  /* zero page, 256B, 8-bit addressed */
#define ZP_SIZE        256

#define IO_BASE        0x0100  /* device page, 256B, 8-bit addressed */
#define IO_SIZE        256

#define RAM_BASE       0x0200  /* unified code + data */
#define RAM_SIZE       32512

#define VRAM_BASE      0x8000  /* video ram */
#define VRAM_SIZE      32768

#define RESET_VECTOR   0x0200

/* Framebuffer occupies the first 16000 bytes of VRAM.
   320x200 at 2bpp = 320 * 200 / 4 = 16000 bytes. */
#define FB_BASE        0x8000
#define FB_WIDTH       320
#define FB_HEIGHT      200
#define FB_BYTES       16000

/* Everything past the framebuffer is general spare, usable for patterns. */
#define GFX_BASE       (FB_BASE + FB_BYTES)   /* 0xBE80 */
#define GFX_SIZE       (VRAM_SIZE - FB_BYTES)  /* 16768 */

/* ------------------------------------------------------------------ stacks */

#define STACK_SIZE     256
#define STACK_MASK     0xff   /* circular: pointer wraps with no error */

/* --------------------------------------------------------------- opcodes */

enum {
	BRK = 0x00,
	INC = 0x01, POP = 0x02, NIP = 0x03, SWP = 0x04, ROT = 0x05, DUP = 0x06,
	OVR = 0x07, EQU = 0x08, NEQ = 0x09, GTH = 0x0a, LTH = 0x0b, JMP = 0x0c,
	JCN = 0x0d, JSR = 0x0e, STH = 0x0f, LDZ = 0x10, STZ = 0x11, LDR = 0x12,
	STR = 0x13, LDA = 0x14, STA = 0x15, DEI = 0x16, DEO = 0x17, ADD = 0x18,
	SUB = 0x19, MUL = 0x1a, DIV = 0x1b, AND = 0x1c, ORA = 0x1d, EOR = 0x1e,
	SFT = 0x1f
};

/* Mode bits. Lower 5 bits are the opcode, upper 3 bits are flags. */
#define MODE_SHORT     0x20    /* 2: operands are 16-bit shorts */
#define MODE_RETURN    0x40    /* r: operate on the return stack */
#define MODE_KEEP      0x80    /* k: do not consume operands */

#define OPCODE_MASK    0x1f

/* Opcode 0x00 is contextual: the meaning depends on the mode bits. */
enum {
	OP_JCI = 0x20,   /* jump to inline addr if popped byte != 0 */
	OP_JMI = 0x40,   /* jump to inline absolute addr */
	OP_JSI = 0x60,   /* jump to inline addr, stash return addr */
	OP_LIT = 0x80,   /* push next byte */
	OP_LIT2 = 0xa0,  /* push next short */
	OP_LITR = 0xc0,  /* push next byte to return stack */
	OP_LIT2R = 0xe0  /* push next short to return stack */
};

/* ------------------------------------------------------------ machine state */

typedef struct {
	Uint8 mem[MEM_SIZE];       /* the whole 64KB address space */
	Uint8 wst[STACK_SIZE];     /* working stack */
	Uint8 rst[STACK_SIZE];     /* return stack */
	Uint8 wstp;                /* working stack pointer */
	Uint8 rstp;                /* return stack pointer */
	Uint16 pc;                 /* 16-bit program counter */
} Dux;

/* Device callbacks. The core calls these for any access to IO_BASE, letting
   the host attach real hardware. Returning 0 for an unhandled port is fine;
   the core falls back to plain memory in that case. */
typedef Uint8 (*DeiFn)(void *ctx, Uint8 addr);
typedef void  (*DeoFn)(void *ctx, Uint8 addr, Uint8 val);

/* ------------------------------------------------------------------- api */

extern Dux dux;

/* Zero all memory, both stacks, both pointers, and the program counter.
   Sets pc to RESET_VECTOR. */
extern void dux_reset(void);

/* Clear the zero page only. Used for a soft reboot, which preserves it. */
extern void dux_soft_reset(void);

/* Execute until BRK, or until `steps` instructions have run. Returns 0 if the
   vector ran to BRK, 1 if the step budget was exhausted. `steps` of 0 means
   unlimited. */
extern int dux_eval(Uint16 pc, unsigned long steps);

/* ------------------------------------------------------------------ memory */

extern Uint8 dux_load(Uint16 addr);
extern void  dux_store(Uint16 addr, Uint8 val);

/* ------------------------------------------------------------------- stack */

/* Both stacks are circular. These wrap rather than faulting; there are no
   invalid programs. */
extern void dux_push(Uint8 val);
extern Uint8 dux_pop(void);

/* ------------------------------------------------------------------ devices */

extern void dux_dei(Uint8 addr);   /* push D[addr] */
extern void dux_deo(Uint8 addr);   /* pop and write to D[addr] */

/* Attach the host's device layer. Passing NULL detaches it and makes the
   device page behave as plain memory. */
extern void dux_attach(void *ctx, DeiFn dei, DeoFn deo);

/* --------------------------------------------------------------- video */

extern void dux_fb_clear(Uint8 index);
extern void dux_fb_pixel(int x, int y, Uint8 index);
extern Uint8 dux_fb_get(int x, int y);

#endif /* DUX_H */