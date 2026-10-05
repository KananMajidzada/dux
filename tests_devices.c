/* tests_devices.c - the Screen device and the device page.
 *
 * These run the real device layer rather than the bare core, since the point
 * is that port writes reach the framebuffer.
 */

#include "dux.h"
#include "screen.h"
#include <stdio.h>

static int checks, failures;

static void expect(const char *name, int got, int want) {
	checks++;
	if(got != want) {
		failures++;
		printf("FAIL %s: got %d, want %d\n", name, got, want);
	}
}

/* Attach a device that forwards the screen range and nothing else, matching
   the host. */
static Uint8 dev_read(void *ctx, Uint8 addr) {
	(void)ctx;
	if(addr >= 0x20 && addr <= 0x2f) return screen_read(addr);
	return dux.mem[IO_BASE + addr];
}

static void dev_write(void *ctx, Uint8 addr, Uint8 val) {
	(void)ctx;
	if(addr >= 0x20 && addr <= 0x2f) { screen_write(addr, val); return; }
	if(addr == 0x0f && val) { /* state: quit, ignored here */ }
}

static void setup(void) {
	dux_reset();
	screen_init();
	dux_attach(0, dev_read, dev_write);
}

/* Ports are reached through dux_store/dux_load, the same path the core uses for
   the device page, so these tests exercise the real routing. */
static void wr(Uint8 port, Uint8 val) { dux_store((Uint16)(IO_BASE + port), val); }
static Uint8 rd(Uint8 port) { return dux_load((Uint16)(IO_BASE + port)); }

/* Write a short device port the way DEO2 does: high byte at the base, low
   byte at base+1, which is the order the core's GET/DEO_BODY pair produces. */
static void wr_short(Uint8 port, Uint16 val) {
	wr(port,   (Uint8)(val >> 8));
	wr(port+1, (Uint8)(val & 0xff));
}

/* A short port is read as a pair, high half first. Reading it through one
   helper keeps the convention in a single place in this file. */
static int x_val(void) { return (screen_read(0x28) << 8) | screen_read(0x29); }
static int y_val(void) { return (screen_read(0x2a) << 8) | screen_read(0x2b); }

int main(void) {
	/* ---------------------------------------------------- device routing */
	/* A byte write to a screen port must reach the framebuffer. This is the
	   case that caught a truncated IO_BASE + port sum. */
	setup();
	wr(0x29, 5);          /* x = 5 */
	wr(0x2e, 2);          /* pixel = colour 2 */
	expect("pixel reaches framebuffer", screen_get(5, 0), 2);
	/* x=5 lives in byte 1 at shift (5&3)*2 = 2, so colour 2 sets 0x08. */
	expect("framebuffer byte set", dux.mem[FB_BASE + 1] & 0x0c, 0x08);

	/* Neighbouring pixels must be untouched. */
	expect("left neighbour clear", screen_get(4, 0), 0);
	expect("right neighbour clear", screen_get(6, 0), 0);

	/* --------------------------------------------------- cursor ports */
	setup();
	wr(0x29, 100);
	wr(0x2b, 50);
	/* A bare write to the low half means the whole value, so the pair reads
	   back as 100 with a clear high half. */
	expect("x value", x_val(), 100);
	expect("x high half clear", screen_read(0x28), 0);
	expect("x low half", screen_read(0x29), 100);
	expect("y value", y_val(), 50);

	/* A short is written high byte first, then low; both must survive. */
	setup();
	wr_short(0x28, 0x012c);           /* x = 300 */
	expect("short write high half", screen_read(0x28), 0x01);
	expect("short write low half", screen_read(0x29), 0x2c);
	/* The assembled value is what the cursor uses. */
	wr(0x2e, 3);
	expect("short x used for drawing", screen_get(0x12c, 0), 3);

	/* The assembler emits a single byte for small ports; that must work. */
	setup();
	wr(0x28, 7);                       /* bare write to the high half */
	expect("byte write set the value", x_val(), 7);
	expect("byte write left the high half clear", screen_read(0x28), 0);

	/* ------------------------------------------------ auto-advance */
	setup();
	wr(0x26, 1);          /* auto on  */
	expect("auto reads back", screen_read(0x26), 1);
	wr(0x29, 10);         /* x = 10, bare low byte */
	wr(0x2e, 3);
	expect("first pixel", screen_get(10, 0), 3);
	wr(0x2e, 3);
	expect("second pixel advanced", screen_get(11, 0), 3);
	wr(0x2e, 3);
	expect("third pixel advanced", screen_get(12, 0), 3);
	expect("cursor advanced", x_val(), 13);

	/* Auto-advance off leaves the cursor put. */
	setup();
	wr(0x26, 0);
	wr(0x29, 4);
	wr(0x2e, 1);
	wr(0x2e, 1);
	expect("no advance first", screen_get(4, 0), 1);
	expect("no advance second overwrote", screen_get(4, 0), 1);
	expect("cursor did not move", x_val(), 4);

	/* Wrapping past the right edge moves to the next row. */
	setup();
	wr(0x26, 1);
	wr_short(0x28, (Uint16)(FB_WIDTH - 1));        /* x = 319 */
	wr_short(0x2a, 4);                             /* y = 4 */
	wr(0x2e, 2);
	wr(0x2e, 2);
	expect("wrapped to next row", screen_get(0, 5), 2);
	expect("cursor x reset", x_val(), 1);

	/* ----------------------------------------------------- fixed sizes */
	setup();
	expect("width high", screen_read(0x22), (FB_WIDTH >> 8) & 0xff);
	expect("width low", screen_read(0x23), FB_WIDTH & 0xff);
	expect("height high", screen_read(0x24), (FB_HEIGHT >> 8) & 0xff);
	expect("height low", screen_read(0x25), FB_HEIGHT & 0xff);

	/* ------------------------------------------------------- sprites
	   A mask byte covers four pixels, two bits each in the same order as the
	   framebuffer: high bits leftmost. Zero is transparent, one to three is
	   that colour, so a sprite can be multi-colour in a single pass. The
	   length field counts mask bytes, not pixels. */
	setup();
	dux.mem[0x0300] = 0x55;   /* four pixels, all colour 1 */
	dux.mem[0x0301] = 0x40;   /* only pixel 0 of this byte */
	wr_short(0x2c, 0x0300);        /* addr = 0x0300 */
	wr(0x29, 20);                      /* x = 20 */
	wr(0x2b, 10);                      /* y = 10 */
	wr(0x2f, 1 << 5);                  /* 2 bytes wide */

	expect("sprite px 0", screen_get(20, 10), 1);
	expect("sprite px 1", screen_get(21, 10), 1);
	expect("sprite px 2", screen_get(22, 10), 1);
	expect("sprite px 3", screen_get(23, 10), 1);
	expect("sprite px 4 second byte", screen_get(24, 10), 1);
	expect("sprite px 5 second byte clear", screen_get(25, 10), 0);
	expect("sprite stops at length", screen_get(26, 10), 0);
	expect("nothing past the sprite", screen_get(27, 10), 0);

	/* Each pixel carries its own colour, which is the point of two bits. */
	setup();
	dux.mem[0x0500] = 0x1b;          /* four pixels: transparent, 1, 2, 3 */
	wr_short(0x2c, 0x0500);
	wr(0x29, 0);
	wr(0x2b, 0);
	wr(0x2f, 1 << 5);                /* one byte, four pixels */
	expect("multi-colour px 0 transparent", screen_get(0, 0), 0);
	expect("multi-colour px 1", screen_get(1, 0), 1);
	expect("multi-colour px 2", screen_get(2, 0), 2);
	expect("multi-colour px 3", screen_get(3, 0), 3);

	/* Bit 7 paints the mask in one flat colour, which is how a shape is
	   stamped. The pixels come from the mask, not from the value. */
	setup();
	dux.mem[0x0500] = 0x3f;          /* transparent, then three lit pixels */
	wr_short(0x2c, 0x0500);
	wr(0x29, 0);
	wr(0x2b, 0);
	wr(0x2f, 0x02 | 0x80 | (1 << 5));   /* flat, colour 2, one byte */
	expect("flat px 0 leaves the transparent alone", screen_get(0, 0), 0);
	expect("flat px 1 takes the flat colour", screen_get(1, 0), 2);
	expect("flat px 2 takes the flat colour", screen_get(2, 0), 2);
	expect("flat px 3 takes the flat colour", screen_get(3, 0), 2);

	/* Bit 7 with colour 0 erases: the same mask, painted in the background. */
	setup();
	dux.mem[0x0500] = 0x3f;          /* transparent, then three lit pixels */
	wr_short(0x2c, 0x0500);
	wr(0x29, 0);
	wr(0x2b, 0);
	wr(0x2f, 0x02 | 0x80 | (1 << 5));   /* paint them first */
	expect("erase setup px 1", screen_get(1, 0), 2);
	wr(0x2f, 0x00 | 0x80 | (1 << 5));   /* then stamp the same mask in colour 0 */
	expect("erase clears px 1", screen_get(1, 0), 0);
	expect("erase clears px 2", screen_get(2, 0), 0);
	expect("erase clears px 3", screen_get(3, 0), 0);

	/* A solid run needs no mode of its own: a mask byte of $ff is four lit
	   pixels, and the flat bit paints them all one colour. */
	setup();
	dux.mem[0x0400] = 0xff;
	wr_short(0x2c, 0x0400);
	wr(0x29, 0);
	wr(0x2b, 0);
	wr(0x2f, 0x02 | 0x80 | (3 << 5));    /* flat, colour 2, four pixels */
	expect("solid run px 0", screen_get(0, 0), 2);
	expect("solid run px 3", screen_get(3, 0), 2);
	expect("solid run stops at length", screen_get(4, 0), 0);

	/* The foreground layer refuses to paint over colour 0. */
	setup();
	wr(0x29, 1); wr(0x2e, 0);           /* background colour at x=1 */
	dux.mem[0x0400] = 0x55;
	wr_short(0x2c, 0x0400);
	wr(0x29, 0);
	wr(0x2f, (1 << 2) | (1 << 5));          /* layer 1 */
	expect("fg layer skips colour 0", screen_get(1, 0), 0);

	/* The background layer paints over anything. */
	setup();
	wr(0x29, 1); wr(0x2e, 0);
	dux.mem[0x0400] = 0x55;
	wr_short(0x2c, 0x0400);
	wr(0x29, 0);
	wr(0x2f, 1 << 5);                       /* layer 0 */
	expect("bg layer paints over colour 0", screen_get(1, 0), 1);

	/* -------------------------------------------------- edge clipping */
	setup();
	wr(0x29, (Uint8)((FB_WIDTH + 10) & 0xff));
	wr(0x2e, 2);
	expect("write past right edge is ignored", screen_get(0, 0), 0);

	wr(0x29, 0);
	wr(0x2b, FB_HEIGHT + 10);
	wr(0x2e, 2);
	expect("write past bottom edge is ignored", screen_get(0, 0), 0);

	/* ------------------------------------------------------- palette */
	setup();
	expect("palette 0 is dark", screen_palette(0)[0] <= 0x40, 1);
	screen_set_colour(2, 0x11, 0x22, 0x33);
	expect("palette 2 red", screen_palette(2)[0], 0x11);
	expect("palette 2 green", screen_palette(2)[1], 0x22);
	expect("palette 2 blue", screen_palette(2)[2], 0x33);

	/* ------------------------------------- store operand order
	   A memory op pops the address from the top, so the assembler pushes the
	   value first. This caught a bug where a one-operand store with an
	   explicit zero value silently wrote zero over the incremented byte. */
	setup();
	wr(0x2e, 0);                       /* x = 0, y = 0 */
	dux.mem[0x11] = 0;
	{
		/* LIT $05 ; STZ with the value left on the stack */
		Uint16 pc = 0x0500;
		dux.mem[pc + 0] = 0x80; dux.mem[pc + 1] = 0x05;  /* LIT $05   */
		dux.mem[pc + 2] = 0x80; dux.mem[pc + 3] = 0x11;  /* LIT $11   */
		dux.mem[pc + 4] = 0x11;                           /* STZ      */
		dux.mem[pc + 5] = 0x00;                           /* BRK      */
		dux_eval(pc, 0);
	}
	expect("stz stored the value", dux.mem[0x11], 5);

	/* A byte store through the core keeps the value it was given. */
	setup();
	wr(0x28, 0); wr(0x2a, 0);
	dux_store((Uint16)0x0300, 0x77);
	dux_store((Uint16)0x0301, 0x88);
	expect("core byte store 1", dux.mem[0x0300], 0x77);
	expect("core byte store 2", dux.mem[0x0301], 0x88);

	/* --------------------------------------------- unhandled ports */
	/* A port no device claims must still behave as memory. */
	setup();
	wr(0x40, 0x5a);
	expect("unclaimed port stores", dux.mem[IO_BASE + 0x40], 0x5a);
	expect("unclaimed port reads back", rd(0x40), 0x5a);

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}