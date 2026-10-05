/* host.c - the device layer, shared by both front ends. */

#include "host.h"
#include "screen.h"
#include "audio.h"
#include "clock.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

/* --------------------------------------------------------- short port order
 *
 * A 16-bit device port pair occupies base and base+1, and the HIGH byte is at
 * the base. DEO2 stores y[0], which is the high half, at the base; DEI2 pushes
 * the base first, which is again the high half. The two agree, which is what
 * matters, but it is the opposite of the stack's own low-byte-on-top order and
 * the opposite of memory, so it is easy to get wrong.
 *
 * These two helpers are the only code that has to know.
 */

static Uint16 port_in(Uint8 base) {
	return (Uint16)((dux.mem[IO_BASE + base] << 8) |
	                dux.mem[IO_BASE + base + 1]);
}

/* ------------------------------------------------------------- device state
 *
 * Devices own their state rather than living in the shadow memory at 0x0100,
 * so a program cannot corrupt a cursor by writing the page directly. Only
 * Controller and Mouse keep values here; Screen keeps its own.
 */

static Uint8  ctl_button;          /* held buttons, MB_* bits       */
static Uint8  ctl_key;             /* last key pressed               */
static int    ctl_pending;         /* a key event is waiting         */

static int    mouse_x, mouse_y;
static int    mouse_scrollx, mouse_scrolly;
static Uint8  mouse_state;         /* held buttons, MB_* bits        */
static int    mouse_pending;       /* motion or button event waiting */

static int    quit_requested;

/* Console input ring. Small on purpose: a program that never drains it should
   not be able to grow the host's memory. */
#define CONQ 256
static Uint8  conq[CONQ];
static int    conq_head, conq_tail;

/* Event queue for printable characters, kept separate from the key path so a
   program's keyboard handler and its text input do not fight. */
static Uint8  textq[CONQ];
static int    textq_head, textq_tail;

/* Console string and number, for the two convenience print ports. */
static Uint16 con_str;
static Uint16 con_num;

/* ------------------------------------------------------------------- queue */

static void q_push(Uint8 *q, int *tail, Uint8 v) {
	int next = (*tail + 1) % CONQ;
	if(next == *tail && *tail == 0) return;   /* full, drop */
	q[*tail] = v;
	*tail = next;
}

static int q_pop(Uint8 *q, int *head, int *tail) {
	int v;
	if(*head == *tail) return -1;
	v = q[*head];
	*head = (*head + 1) % CONQ;
	return v;
}

static int q_count(int head, int tail) {
	return (tail - head + CONQ) % CONQ;
}

/* ---------------------------------------------------------------- palette
 *
 * The Palette device at 0x40 gives each of the four framebuffer colours one
 * short, packed as 0x0RGB: four bits each for red, green and blue. One write
 * per colour is worth a great deal more than a red/green/blue trio would be,
 * and twelve bits is exactly what the two-bpp framebuffer can show.
 *
 *   0x40/0x41  colour 0      0x44/0x45  colour 2
 *   0x42/0x43  colour 1      0x46/0x47  colour 3
 *
 * System/r, /g and /b at 0x08, 0x0a and 0x0c remain, and now edit colour 1 one
 * component at a time rather than overwriting the other two with zero.
 */

static Uint16 palette[4];

static Uint8 nib(Uint8 v) { return (Uint8)(v * 0x11); }  /* 0..15 -> 0..255 */

static void palette_store(int i, Uint16 v);

void palette_init(void) {
	/* Black, white, and two greys. A program is expected to set its own, but
	   the defaults have to be visible or a blank program looks broken - and a
	   blank program should look like a blank program, which on this machine
	   means black rather than the near-black navy it used to be. The four steps
	   are far enough apart that a figure drawn in all of them reads. */
	palette[0] = 0x0000;
	palette[1] = 0x0fff;
	palette[2] = 0x0999;
	palette[3] = 0x0555;

	{
		int i;
		for(i = 0; i < 4; i++) palette_store(i, palette[i]);
	}
}

/* Store a colour and push it out to the Screen device. The shadow pair is
   also written, so a port read returns what was written even when the write
   arrived a half at a time. */
static void palette_store(int i, Uint16 v) {
	i &= 3;
	palette[i] = v;
	dux.mem[IO_BASE + 0x40 + i * 2]     = (Uint8)(v >> 8);
	dux.mem[IO_BASE + 0x40 + i * 2 + 1] = (Uint8)v;
	/* Mask each nibble before widening. Truncating to a byte first is wrong
	   in a way that is easy to miss: 0x0f0f >> 4 is 0x0f0, which as a byte
	   is 0xf0, so green would come out as 240 instead of 255. */
	screen_set_colour(i,
	                  nib((Uint8)((v >> 8) & 0x0f)),
	                  nib((Uint8)((v >> 4) & 0x0f)),
	                  nib((Uint8)(v & 0x0f)));
}

static Uint8 palette_read(Uint8 addr) {
	Uint16 v = port_in((Uint8)(addr & 0xfe));
	return (addr & 1) ? (Uint8)v : (Uint8)(v >> 8);
}

static void palette_write(Uint8 addr, Uint8 val) {
	Uint8 base = (Uint8)(addr & 0xfe);
	dux.mem[IO_BASE + addr] = val;
	palette_store((base - 0x40) >> 1, port_in(base));
}

void host_colour(int index, Uint8 *r, Uint8 *g, Uint8 *b) {
	const Uint8 *p = screen_palette(index);
	*r = p[0];
	*g = p[1];
	*b = p[2];
}

const Uint8 *host_framebuffer(void) {
	return dux.mem + FB_BASE;
}

/* -------------------------------------------------------------- lifecycle */

void host_init(void) {
	dux_reset();
	screen_init();
	palette_init();
	audio_init();
	file_init();
	dux_attach(0, host_dei, host_deo);

	ctl_button = ctl_key = 0;
	ctl_pending = 0;
	mouse_x = mouse_y = 0;
	mouse_scrollx = mouse_scrolly = 0;
	mouse_state = 0;
	mouse_pending = 0;
	quit_requested = 0;
	conq_head = conq_tail = 0;
	textq_head = textq_tail = 0;
	con_str = con_num = 0;
}

void host_reset(void) {
	/* Sample first: a program is entitled to ask the time while setting
	   itself up, not only once its frame vector is running. */
	clock_tick();
	dux_eval(dux.pc, 0);
}

int host_quit_requested(void) { return quit_requested; }

/* ------------------------------------------------------------ device ports */

Uint8 host_dei(void *ctx, Uint8 addr);

Uint8 host_dei(void *ctx, Uint8 addr) {
	(void)ctx;
	if(addr >= 0x20 && addr <= 0x2f) return screen_read(addr);
	if(addr >= 0x30 && addr <= 0x3f) return audio_read(addr);
	if(addr >= 0x40 && addr <= 0x47) return palette_read(addr);
	if(addr >= 0xa0 && addr <= 0xaf) return file_read(addr);
	if(addr >= 0xc0 && addr <= 0xcf) return clock_read(addr);
	switch(addr) {
	case 0x04: return dux.wstp;                       /* System/wst  */
	case 0x05: return dux.rstp;                       /* System/rst  */
	case 0x08: return nib((Uint8)(palette[1] >> 8));   /* System/red  */
	case 0x09: return nib((Uint8)(palette[1] >> 4));   /* System/green*/
	case 0x0a: return nib((Uint8)palette[1]);          /* System/blue */
	case 0x12: return (Uint8)host_console_pending();   /* Console/read*/
	case 0x14: return (Uint8)(con_str >> 8);            /* Console/str  */
	case 0x15: return (Uint8)con_str;
	case 0x1a: return (Uint8)(con_num >> 8);            /* Console/num  */
	case 0x1b: return (Uint8)con_num;
	case 0x1d: return (Uint8)con_num;
	case 0x8c: return ctl_key;                        /* Ctrl/key    */
	case 0x8d: return ctl_button;                     /* Ctrl/button */
	case 0x92: return mouse_state;                    /* Mouse/state   */
	case 0x93: return (Uint8)mouse_x;                 /* Mouse/x low   */
	case 0x94: return (Uint8)(mouse_x >> 8);
	case 0x95: return (Uint8)mouse_y;                 /* Mouse/y low   */
	case 0x96: return (Uint8)(mouse_y >> 8);
	case 0x99: return (Uint8)mouse_scrollx;           /* Mouse/scrollx */
	case 0x9a: return (Uint8)mouse_scrolly;           /* Mouse/scrolly */
	default:   return dux.mem[IO_BASE + addr];
	}
}

void host_deo(void *ctx, Uint8 addr, Uint8 val);

void host_deo(void *ctx, Uint8 addr, Uint8 val) {
	(void)ctx;
	if(addr >= 0x20 && addr <= 0x2f) { screen_write(addr, val); return; }
	if(addr >= 0x30 && addr <= 0x3f) { audio_write(addr, val); return; }
	if(addr >= 0x40 && addr <= 0x47) { palette_write(addr, val); return; }
	if(addr >= 0xa0 && addr <= 0xaf) { file_write(addr, val); return; }
	switch(addr) {
	case 0x08: palette_store(1, (Uint16)((palette[1] & 0x0fff) |
	                                      ((Uint16)(val >> 4) << 8)));
	           break;
	case 0x0a: palette_store(1, (Uint16)((palette[1] & 0xff0f) |
	                                      ((Uint16)(val >> 4) << 4)));
	           break;
	case 0x0c: palette_store(1, (Uint16)((palette[1] & 0xf0ff) |
	                                      (Uint16)(val >> 4)));
	           break;
	case 0x0e:                                    /* System/debug */
		if(val) fprintf(stderr, "[debug %02x]\n", val);
		break;
	case 0x0f:                                    /* System/state */
		if(val) quit_requested = 1;
		break;
	case 0x14: case 0x15:                               /* Console/str  */
		dux.mem[IO_BASE + addr] = val;
		con_str = port_in(0x14);
		break;

	case 0x16:                                    /* Console/print */
		/* Print a NUL-terminated string from memory. The ISA has no
		   stack-indirect load, so without this every message would have to
		   be unrolled into one port write per character, which would cost
		   more bytecode than the program itself. */
		host_print_str(con_str);
		break;

	case 0x18:                                    /* Console/send */
		putchar(val);
		break;

	case 0x1a: case 0x1b:                               /* Console/num  */
		dux.mem[IO_BASE + addr] = val;
		con_num = port_in(0x1a);
		break;

	case 0x1c:                                    /* Console/number */
		host_print_num(con_num);
		break;

	case 0x1d:                                    /* Console/digit */
		/* Print the byte on top of the stack, taking the high half from the
		   number ports. A device write in short mode pops two bytes, so a
		   program holding only a byte must not reach for DEO2 here: this
		   port takes its value straight off the stack instead. */
		host_print_num((Uint16)((con_num & 0xff00) | val));
		break;

	case 0x1e:                                    /* Console/newline */
		putchar('\n');
		break;

	case 0x8d:                                    /* Ctrl/button */
		ctl_button = val;
		break;
	default:
		/* No device claims this port, so it behaves as memory. */
		dux.mem[IO_BASE + addr] = val;
		break;
	}
}

/* ------------------------------------------------------------ input feed */

void host_key_down(Uint8 code) {
	ctl_key = code;
	ctl_button |= (Uint8)(1 << 7);   /* the key bit, as the port reports it */
	ctl_pending = 1;
}

void host_key_up(Uint8 code) {
	(void)code;
	ctl_button &= (Uint8)~(1 << 7);   /* clear the key bit */
	ctl_pending = 1;
}

void host_mouse_move(int dx, int dy) {
	mouse_x += dx;
	mouse_y += dy;
	if(mouse_x < 0) mouse_x = 0;
	if(mouse_y < 0) mouse_y = 0;
	if(mouse_x >= FB_WIDTH)  mouse_x = FB_WIDTH - 1;
	if(mouse_y >= FB_HEIGHT) mouse_y = FB_HEIGHT - 1;
	mouse_pending = 1;
}

void host_mouse_button(int down, int buttons) {
	(void)down;
	mouse_state = buttons;      /* the full set currently held */
	mouse_pending = 1;
}

void host_scroll(int dx, int dy) {
	mouse_scrollx = dx;
	mouse_scrolly = dy;
	mouse_pending = 1;
}

void host_text(int ch) {
	q_push(textq, &textq_tail, (Uint8)ch);
	q_push(conq, &conq_tail, (Uint8)ch);
}

/* Print the NUL-terminated string at addr. Stops at the end of memory as well
   as at the NUL, so a pointer that was never set cannot run away. */
void host_print_str(Uint16 addr) {
	Uint32 i;
	for(i = addr; i < MEM_SIZE && dux.mem[i]; i++)
		putchar(dux.mem[i]);
}

/* Print an unsigned 16-bit value in decimal, with no leading zeros. */
void host_print_num(Uint16 v) {
	char buf[6];
	int n = 0;
	if(v == 0) { putchar('0'); return; }
	while(v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
	while(n) putchar(buf[--n]);
}

int host_console_read(void) { return q_pop(conq, &conq_head, &conq_tail); }
int host_console_pending(void) { return q_count(conq_head, conq_tail); }

/* ---------------------------------------------------------------- frames */

/* Run the vector of a device, if it has one and it is non-zero.
 *
 * A device's ports sit at the device number itself: device 0x80 owns 0x80
 * through 0x8f, so its vector pair is at 0x80 and 0x81. Screen keeps its own
 * vector because its port writes never reach the shadow memory.
 */
/* A vector gets a step budget. A program with a loop that never ends would
   otherwise hang the host outright, and there is no way to recover from that
   except killing the process: the vector has to return to the frame loop for
   anything to happen again.

   The budget is per frame, so it has to cover the heaviest thing a frame does.
   Filling six faces of a spinning cube plots some eleven thousand pixels, and a
   pixel costs a couple of dozen steps through the line drawer, which is already
   a third of a million. Two million leaves room without making a runaway loop
   take noticeably longer to notice. */
#define VECTOR_STEPS 8000000

static int runaway;

static void run_vector(Uint8 dev) {
	Uint16 vec;
	if(dev == 0x20) {
		vec = screen_vector();
	} else {
		vec = port_in(dev);
	}
	if(!vec) return;
	if(dux_eval(vec, VECTOR_STEPS) && !runaway) {
		runaway = 1;
		fprintf(stderr,
		        "host: vector %02x at %04x did not reach BRK in %d steps;"
		        " the frame was cut short\n",
		        dev, (int)vec, VECTOR_STEPS);
	}
	/* A vector is a subroutine: it owes the host a balanced working stack.
	   Reporting it is worth a line of code, because an unbalanced vector
	   wraps a 256-byte stack, and the program then misbehaves in ways that
	   look nothing like the mistake that caused them. */
	if(getenv("DUX_REPORT_DEPTH")) {
		/* Per frame rather than latched: a vector that leaks keeps leaking,
		   and the rate is what says whether the fault is a loop that runs a
		   fixed number of times or one that leaks per iteration. */
		fprintf(stderr, "depth after vector %02x: %d\n", dev, (int)dux.wstp);
		return;
	}
	if(dux.wstp && !runaway) {
		runaway = 1;
		fprintf(stderr,
		        "host: vector %02x at %04x left %d items on the stack;"
		        " it should end balanced\n",
		        dev, (int)vec, (int)dux.wstp);
	}
}

int host_frame(void) {
	/* The reset vector runs once, and installs the device vectors. */
	/* (host_reset is called separately by the front end.) */

	/* Input first, so a program sees the position that caused the event. */
	run_vector(0xa0);                           /* File, if armed */
	if(ctl_pending) {
		ctl_pending = 0;
		run_vector(0x80);                       /* Controller */
	}
	if(mouse_pending) {
		mouse_pending = 0;
		mouse_scrollx = mouse_scrolly = 0;
		run_vector(0x90);                       /* Mouse      */
	}

	clock_tick();                              /* sample the wall clock */
	run_vector(0x20);                           /* Screen     */

	return !quit_requested;
}