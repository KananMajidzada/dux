/* screen.c - the Screen device, and the framebuffer blitter.
 *
 * Device 0x20, sixteen ports. The framebuffer itself lives in video RAM at
 * 0x8000; this device only moves a cursor and interprets draw commands.
 *
 * Ports, per ARCHITECTURE.md:
 *   0x20  vector*   evaluated once per frame
 *   0x22  width*    readable, 320
 *   0x24  height*   readable, 200
 *   0x26  auto      increment x, or y on wrap
 *   0x28  x*        cursor column
 *   0x2a  y*        cursor row
 *   0x2c  addr*     address of sprite data, or of the pixel to write
 *   0x2e  pixel     write one pixel
 *   0x2f  sprite    draw a sprite
 *
 * Short ports are read and written high byte first. Only the low byte of a
 * short write triggers behaviour, which is why a two-byte write to a cursor
 * port lands here as two separate calls.
 *
 * DUX_TRACE_PIXEL=1 prints every pixel the port paints, with the coordinate it
 * actually used and a note when a coordinate came from a half-written short.
 * DUX_TRACE_SPRITE=1 does the same for sprites. Neither prints unless asked,
 * and both exist because a picture cannot say where a missing pixel went: a
 * ROM that draws three thousand of them and shows fifteen is correct as far as
 * its own arithmetic goes, and the only way to see that all three thousand
 * reached the device and two thousand nine hundred and eighty-five were erased
 * afterwards is to read the port's own list.
 */

#include "screen.h"
#include <stdio.h>
#include <stdlib.h>

/* The device owns its cursor, palette and sprite state, so a program cannot
   corrupt it by writing the device page directly. */
typedef struct {
	Uint16 x, y;
	Uint16 addr;        /* sprite/pixel data address */
	int   auto_advance;
	Uint8 palette[4][3];
} Screen;

static Screen scr;

static Uint16 pending;
static Uint8  high_x, high_y, high_a, high_vec;
static int    x_pending, y_pending, a_pending, vec_pending;

/* The vector address the program installed. */
Uint16 screen_vector(void) {
	/* A pending half means only the high byte has arrived, which on its own
	   is not an address a program would want jumped to. Reporting zero means
	   the host simply does not run the vector yet, rather than running it
	   from the wrong place. */
	if(vec_pending) return 0;
	return pending;
}


void screen_init(void) {
	scr.x = 0;
	scr.y = 0;
	scr.addr = 0;
	scr.auto_advance = 0;
	x_pending = y_pending = a_pending = vec_pending = 0;
	/* A legible default palette: near-black ground, then three hues. */
	scr.palette[0][0] = 0x10; scr.palette[0][1] = 0x10; scr.palette[0][2] = 0x18;
	scr.palette[1][0] = 0xe0; scr.palette[1][1] = 0x60; scr.palette[1][2] = 0x90;
	scr.palette[2][0] = 0xf0; scr.palette[2][1] = 0xd0; scr.palette[2][2] = 0x60;
	scr.palette[3][0] = 0x60; scr.palette[3][1] = 0xd0; scr.palette[3][2] = 0xf0;
}

/* Expose the cursor so the host can reset it between vectors if it wants to. */
void screen_cursor(Uint16 *x, Uint16 *y) {
	if(x) *x = scr.x;
	if(y) *y = scr.y;
}

/* --------------------------------------------------------------- drawing */

void screen_plot(int x, int y, Uint8 index) {
	Uint16 addr;
	Uint8  shift;
	Uint8  mask;

	if(x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
	addr  = (Uint16)(FB_BASE + (y * FB_WIDTH + x) / 4);
	shift = (Uint8)((x & 3) * 2);
	mask  = (Uint8)(0x3 << shift);

	/* Clear just this pixel's two bits, then set them. Everything else in
	   the byte, covering three other pixels, must survive. */
	dux.mem[addr] = (Uint8)((dux.mem[addr] & ~mask) |
	                        ((Uint8)(index & 0x3) << shift));
}

Uint8 screen_get(int x, int y) {
	Uint16 addr;
	Uint8  shift;
	if(x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return 0;
	addr  = (Uint16)(FB_BASE + (y * FB_WIDTH + x) / 4);
	shift = (Uint8)((x & 3) * 2);
	return (Uint8)((dux.mem[addr] >> shift) & 0x3);
}

/* Move the cursor one step, wrapping to the next row and then back to the
   top once it leaves the framebuffer. */
static void advance_cursor(void) {
	scr.x++;
	if(scr.x >= FB_WIDTH) {
		scr.x = 0;
		scr.y++;
		if(scr.y >= FB_HEIGHT) scr.y = 0;
	}
}

/* ------------------------------------------------------------------ ports */

Uint16 screen_width(void)  { return (Uint16)FB_WIDTH; }
Uint16 screen_height(void) { return (Uint16)FB_HEIGHT; }

/* Read a Screen port. addr is the full device address (0x20..0x2f).
 *
 * A short port delivers its high byte at the base address, matching DEI2 and
 * every other device on the page. */
Uint8 screen_read(Uint8 addr) {
	switch(addr) {
	case 0x22: return (Uint8)(FB_WIDTH >> 8);
	case 0x23: return (Uint8)(FB_WIDTH & 0xff);
	case 0x24: return (Uint8)(FB_HEIGHT >> 8);
	case 0x25: return (Uint8)(FB_HEIGHT & 0xff);
	case 0x26: return (Uint8)(scr.auto_advance ? 1 : 0);
	case 0x28: return (Uint8)(scr.x >> 8);
	case 0x29: return (Uint8)scr.x;
	case 0x2a: return (Uint8)(scr.y >> 8);
	case 0x2b: return (Uint8)scr.y;
	case 0x2c: return (Uint8)(scr.addr >> 8);
	case 0x2d: return (Uint8)scr.addr;
	default:   return dux.mem[IO_BASE + addr];
	}
}

/* Write a Screen port.
 *
 * A short port has its high byte at the base address and its low byte at
 * base+1, because that is how the core orders a device pair.
 *
 * The assembler emits a bare single-byte write when the value fits in a byte,
 * which has to mean the whole value, so the value is applied the moment any
 * half lands and only combined if the other half arrives afterwards. That way
 * a pending byte still draws, and DEO2 still produces the right short. */
void screen_write(Uint8 addr, Uint8 val) {
	switch(addr) {
	/* auto is a single byte and acts immediately. */
	case 0x26: scr.auto_advance = val ? 1 : 0; break;

	case 0x28: high_x = val; scr.x = (Uint16)val; x_pending = 1; return;
	case 0x29:
		scr.x = x_pending ? (Uint16)(((Uint16)high_x << 8) | val) : (Uint16)val;
		x_pending = 0;
		return;
	case 0x2a: high_y = val; scr.y = (Uint16)val; y_pending = 1; return;
	case 0x2b:
		scr.y = y_pending ? (Uint16)(((Uint16)high_y << 8) | val) : (Uint16)val;
		y_pending = 0;
		return;
	case 0x2c: high_a = val; scr.addr = (Uint16)val; a_pending = 1; return;
	case 0x2d:
		scr.addr = a_pending ? (Uint16)(((Uint16)high_a << 8) | val)
		                     : (Uint16)val;
		a_pending = 0;
		return;

	case 0x20: high_vec = val; vec_pending = 1; return;
	case 0x21:
		pending = vec_pending ? (Uint16)(((Uint16)high_vec << 8) | val)
		                      : (Uint16)val;
		vec_pending = 0;
		return;
	case 0x25: return;                   /* height, read only */
	case 0x27: return;                   /* reserved */

	case 0x2e: {                          /* pixel */
		if(getenv("DUX_TRACE_PIXEL"))
			fprintf(stderr, "pixel x=%u%s y=%u%s val=%u\n",
			        (unsigned)(x_pending ? (Uint16)(scr.x & 0xff) : scr.x),
			        x_pending ? "(pending)" : "",
			        (unsigned)(y_pending ? (Uint16)(scr.y & 0xff) : scr.y),
			        y_pending ? "(pending)" : "", (unsigned)val);
		screen_plot((int)(x_pending ? (Uint16)(scr.x & 0xff) : scr.x),
		            (int)(y_pending ? (Uint16)(scr.y & 0xff) : scr.y),
		            val);
		if(scr.auto_advance) advance_cursor();
		break;
	}

	case 0x2f: {                          /* sprite */
		/* The value packs the shape, the layer and the colour override:
		     bits 0-1  colour used when the flat bit is set
		     bit  2    layer: 0 paints over anything, 1 keeps colour 0
		     bit  3    flip horizontally
		     bit  4    flip vertically
		     bits 5-6  length, plus one
		     bit  7    flat: paint the mask in the colour above

		   A sprite reads length bytes from addr, four pixels each, two bits per
		   pixel in the same order as the framebuffer. Zero is transparent; one,
		   two or three is that colour. Letting the data carry the colour is what
		   makes a multi-colour sprite a single pass instead of one call per
		   colour, which is the whole point of a two-bit framebuffer.

		   The flat bit paints every lit pixel in one colour instead, which is
		   how a shape is stamped and, with colour 0, erased. It replaces what
		   used to be a separate "filled bar" mode: a solid run is just a mask
		   byte of $ff, so the extra mode bought nothing, and spending the bit
		   on stamping buys the erase that any moving sprite needs. */
		Uint8 colour = (Uint8)(val & 0x3);
		Uint8 layer  = (Uint8)((val >> 2) & 1);
		Uint8 flipx  = (Uint8)((val >> 3) & 1);
		Uint8 flipy  = (Uint8)((val >> 4) & 1);
		Uint8 flat   = (Uint8)((val >> 7) & 1);
		int   len    = (int)((val >> 5) & 0x3) + 1;
		int   i, j, row;

		/* Walk the pixels the sprite covers, left to right. flip-y mirrors the
		   run onto the row below, which is all a 1-D mask can express; a taller
		   sprite needs one call per row. */
		for(row = 0; row < (flipy ? 2 : 1); row++) {
			int py = (int)scr.y + row;
			for(i = 0; i < len; i++) {
				Uint8 mask = dux.mem[(Uint16)(scr.addr + i)];
				int base = flipx ? (len - 1 - i) * 4 : i * 4;
				for(j = 0; j < 4; j++) {
					int qx = (int)scr.x + base + j;
					int v = (mask >> (6 - j * 2)) & 0x3;
					if(!v) continue;                   /* transparent */
					if(flat) v = colour;
					if(layer == 1 && screen_get(qx, py) == 0) continue;
					screen_plot(qx, py, (Uint8)v);
				}
			}
		}
		if(scr.auto_advance) advance_cursor();
		/* Ground truth for a sprite that draws the wrong shape. A rendered
		   picture cannot say whether a run came out five pixels wide because
		   the mask is wrong or because the cursor was somewhere else; this
		   prints every field the loop above is about to use. Off unless
		   DUX_TRACE_SPRITE is set, so it costs nothing when unset. */
		if(getenv("DUX_TRACE_SPRITE"))
			fprintf(stderr, "sprite x=%d y=%d len=%d addr=%04x val=%02x "
			                "mask=%02x %02x\n",
			        (int)scr.x, (int)scr.y, len, (int)scr.addr, (int)val,
			        (int)dux.mem[(Uint16)scr.addr],
			        len > 1 ? (int)dux.mem[(Uint16)(scr.addr + 1)] : 0);
		break;
	}

	default:
		dux.mem[IO_BASE + addr] = val;
	}
}

/* Palette access, used by the System device's colour ports. */
void screen_set_colour(int index, Uint8 r, Uint8 g, Uint8 b) {
	if(index < 0 || index > 3) return;
	scr.palette[index][0] = r;
	scr.palette[index][1] = g;
	scr.palette[index][2] = b;
}

const Uint8 *screen_palette(int index) {
	if(index < 0 || index > 3) return scr.palette[0];
	return scr.palette[index];
}

/* Draw a filled rectangle. A convenience for the host, not reachable from
   the ISA; programs use the pixel and sprite ports. */
void screen_rect(int x, int y, int w, int h, Uint8 index) {
	int j, i;
	for(j = 0; j < h; j++)
		for(i = 0; i < w; i++)
			screen_plot(x + i, y + j, index);
}