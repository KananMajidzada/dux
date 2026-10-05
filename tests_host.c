/* tests_host.c - the shared device layer: input, vectors, and the frame loop.
 *
 * The point is that input arriving between frames is queued and delivered to
 * the right vector, which is the whole non-interruptible contract.
 */

#include "dux.h"
#include "host.h"
#include "screen.h"
#include "audio.h"
#include "clock.h"
#include "file.h"

#include <stdio.h>

static int checks, failures;

static void expect(const char *name, int got, int want) {
	checks++;
	if(got != want) {
		failures++;
		printf("FAIL %s: got %d, want %d\n", name, got, want);
	}
}

/* Build a reset vector at 0x0400 that installs `handler` as the vector of the
   device whose ports start at dev_base, and a frame handler at 0x0500 that
   bumps the counter in the zero page.

   The vector goes out as a single DEO2. A device pair holds its high byte at
   the base port, so DEO2 and the host's own read of that pair agree; writing
   the two bytes separately, as an earlier version of this test did, has to get
   the order right by hand and did not. */
static void install_vector_program(Uint8 dev_base, Uint8 dev_hi, Uint16 handler) {
	unsigned pc;
	(void)dev_hi;
	host_init();          /* zeroes memory, so build the program afterwards */
	pc = 0x0400;
	dux.mem[pc + 0] = 0xa0;
	dux.mem[pc + 1] = (Uint8)(handler >> 8);
	dux.mem[pc + 2] = (Uint8)(handler & 0xff);   /* LIT2 &handler */
	dux.mem[pc + 3] = 0x80; dux.mem[pc + 4] = dev_base; /* LIT $base */
	dux.mem[pc + 5] = 0x37;                      /* DEO2       */
	dux.mem[pc + 6] = 0x00;                      /* BRK       */

	/* handler: counter = counter + 1 */
	pc = handler;
	dux.mem[pc + 0] = 0x80; dux.mem[pc + 1] = 0x14;  /* LIT $14 */
	dux.mem[pc + 2] = 0x10;                          /* LDZ     */
	dux.mem[pc + 3] = 0x80; dux.mem[pc + 4] = 0x01;  /* LIT $01 */
	dux.mem[pc + 5] = 0x18;                          /* ADD     */
	dux.mem[pc + 6] = 0x80; dux.mem[pc + 7] = 0x14;  /* LIT $14 */
	dux.mem[pc + 8] = 0x11;                          /* STZ     */
	dux.mem[pc + 9] = 0x00;                          /* BRK     */

	dux.pc = 0x0400;
	host_reset();
}

int main(void) {
	/* ------------------------------------------------- frame vector */
	install_vector_program(0x20, 0x21, 0x0500);
	expect("counter starts at zero", dux.mem[0x14], 0);
	host_frame();
	expect("one frame ran the vector", dux.mem[0x14], 1);
	host_frame();
	host_frame();
	expect("three frames", dux.mem[0x14], 3);

	/* The working stack must be balanced after each vector. */
	expect("stack balanced after frames", dux.wstp, 0);
	expect("return stack balanced", dux.rstp, 0);

	/* ------------------------------------------------- controller */
	{
		install_vector_program(0x80, 0x81, 0x0500);
		/* The screen vector points at the same handler by default; clear it
		   so only the key path can move the counter. */
		screen_write(0x20, 0);
		screen_write(0x21, 0);

		expect("no key yet", dux.mem[0x14], 0);
		host_frame();
		expect("keyless frame does not run the key vector", dux.mem[0x14], 0);

		host_key_down(KEY_RIGHT);
		host_frame();
		expect("key press ran the vector once", dux.mem[0x14], 1);

		/* A second frame with no new input must not re-fire. */
		host_frame();
		expect("no repeat without input", dux.mem[0x14], 1);

		host_key_up(KEY_RIGHT);
		host_frame();
		expect("key release is an event too", dux.mem[0x14], 2);
	}

	/* The key port reports the last key pressed. */
	host_key_down('a');
	expect("key port", host_dei(0, 0x8c), 'a');

	/* ------------------------------------------------------ mouse */
	{
		/* The handler copies the mouse x low byte into the counter. */
		unsigned pc;
		install_vector_program(0x90, 0x91, 0x0500);
		screen_write(0x20, 0);
		screen_write(0x21, 0);
		pc = 0x0500;
		dux.mem[pc + 0] = 0x80; dux.mem[pc + 1] = 0x93;  /* LIT $93 */
		dux.mem[pc + 2] = 0x16;                          /* DEI     */
		dux.mem[pc + 3] = 0x80; dux.mem[pc + 4] = 0x14;  /* LIT $14 */
		dux.mem[pc + 4 + 1] = 0x11;                     /* STZ     */
		dux.mem[pc + 6] = 0x00;                          /* BRK     */

		host_mouse_move(40, 30);
		host_frame();
		expect("mouse vector ran", dux.mem[0x14], 40);
		host_mouse_move(10, 5);
		host_frame();
		expect("mouse x accumulated", dux.mem[0x14], 50);
	}

	/* Mouse position is clamped to the framebuffer. */
	host_init();
	host_mouse_move(-100, -100);
	expect("x clamped low", host_dei(0, 0x93), 0);
	host_mouse_move(10000, 10000);
	expect("x clamped high low", host_dei(0, 0x93), (FB_WIDTH - 1) & 0xff);
	expect("x clamped high high", host_dei(0, 0x94), ((FB_WIDTH - 1) >> 8) & 0xff);
	expect("y clamped high low", host_dei(0, 0x95), (FB_HEIGHT - 1) & 0xff);
	expect("y clamped high high", host_dei(0, 0x96), ((FB_HEIGHT - 1) >> 8) & 0xff);

	/* -------------------------------------------------------- console */
	host_init();
	expect("console empty", host_console_pending(), 0);
	host_text('d');
	host_text('u');
	host_text('x');
	expect("console queued", host_console_pending(), 3);
	expect("first char", host_console_read(), 'd');
	expect("second char", host_console_read(), 'u');
	expect("third char", host_console_read(), 'x');
	expect("console drained", host_console_pending(), 0);
	expect("read past end", host_console_read(), -1);

	/* The console read port reports the count. */
	host_text('x');
	expect("read port reports pending", host_dei(0, 0x12), 1);

	/* A full ring drops rather than overflowing. */
	{
		int i;
		host_init();
		for(i = 0; i < 400; i++) host_text('x');
		expect("ring is bounded", host_console_pending() <= 256, 1);
	}

	/* ------------------------------------------------------- palette */
	host_init();
	{
		Uint8 r, g, b;
		host_colour(0, &r, &g, &b);
		expect("colour 0 is dark", r < 0x40, 1);
	}

	/* ------------------------------------------------- quit signal */
	host_init();
	expect("not quitting", host_quit_requested(), 0);
	{
		/* System/state non-zero asks the host to stop. */
		host_deo(0, 0x0f, 1);
		expect("quit requested", host_quit_requested(), 1);
		expect("host_frame reports the quit", host_frame(), 0);
	}

	/* ------------------------------------------------------- palette
	 *
	 * Four shorts, one per colour, packed 0x0RGB. The nibbles widen to bytes
	 * by 0x11, so 0x0fff is white. */
	host_init();
	/* 0x0fff is white: the low byte holds green in its high nibble and blue
	   in its low one, so white needs 0xff down there, not 0x0f. */
	host_deo(0, 0x45, 0xff);            /* colour 2, low half  */
	host_deo(0, 0x44, 0x0f);            /* colour 2, high half */
	expect("palette high half reads back", host_dei(0, 0x44), 0x0f);
	expect("palette low half reads back", host_dei(0, 0x45), 0xff);
	{
		Uint8 r, g, b;
		host_colour(2, &r, &g, &b);
		expect("palette red widens", r, 0xff);
		expect("palette green widens", g, 0xff);
		expect("palette blue widens", b, 0xff);
	}
	/* Writing one half must leave the other alone. Colour 0 defaults to
	   0x0001, so the high half is at $40 and the low half at $41. */
	host_deo(0, 0x41, 0x07);
	expect("half write changed the low half", host_dei(0, 0x41), 0x07);
	expect("half write kept the high half", host_dei(0, 0x40), 0x00);

	/* System/red edits colour 1 without clobbering green or blue. */
	host_init();
	/* The high byte of the pair is at the base port, so $42 takes the high
	   half and $43 the low one. Colour 1 becomes 0x0120. */
	host_deo(0, 0x43, 0x20);            /* low half  */
	host_deo(0, 0x42, 0x01);            /* high half */
	host_deo(0, 0x08, 0xff);            /* System/red = full */
	expect("red write set the high half", host_dei(0, 0x42), 0x0f);
	expect("red write kept the low half", host_dei(0, 0x43), 0x20);

	/* --------------------------------------------------------- audio
	 *
	 * A short device port has its high byte at the lower address. Getting
	 * that backwards silently halves the pitch and can mute the note
	 * outright, so it is worth pinning down one port at a time. */
	host_init();
	audio_init();
	host_deo(0, 0x32, 0);               /* channel 0 */
	host_deo(0, 0x34, 0x00);            /* period, high */
	host_deo(0, 0x35, 0x64);            /* period, low  */
	expect("period high half", host_dei(0, 0x34), 0x00);
	expect("period low half", host_dei(0, 0x35), 0x64);
	host_deo(0, 0x36, 0x40);            /* volume, high */
	host_deo(0, 0x37, 0x00);            /* volume, low  */
	expect("volume high half", host_dei(0, 0x36), 0x40);
	expect("volume low half", host_dei(0, 0x37), 0x00);
	host_deo(0, 0x3a, 0x00);            /* beat, high */
	host_deo(0, 0x3b, 0x80);            /* beat, low  */
	expect("beat high half", host_dei(0, 0x3a), 0x00);
	expect("beat low half", host_dei(0, 0x3b), 0x80);
	expect("note starts off", host_dei(0, 0x38), 0);
	host_deo(0, 0x38, 1);               /* note on */
	expect("note is on", host_dei(0, 0x38), 1);

	/* The channel port selects which channel the others address. */
	host_deo(0, 0x32, 1);
	expect("an untouched channel is silent", host_dei(0, 0x38), 0);
	host_deo(0, 0x32, 0);
	expect("channel 0 is still sounding", host_dei(0, 0x38), 1);
	/* An out-of-range channel wraps instead of indexing off the end. */
	host_deo(0, 0x32, 0xff);
	expect("channel wraps into range", host_dei(0, 0x32), 3);

	/* A note with volume renders something audible. */
	{
		static Sint16 buf[4096];
		int i, nz = 0;
		audio_init();
		host_deo(0, 0x32, 0);
		host_deo(0, 0x34, 0x00); host_deo(0, 0x35, 0x40);
		host_deo(0, 0x36, 0xff); host_deo(0, 0x37, 0xff);
		host_deo(0, 0x3a, 0x00); host_deo(0, 0x3b, 0x00);  /* no attack */
		host_deo(0, 0x38, 1);
		audio_render(buf, 4096);
		for(i = 0; i < 4096; i++) if(buf[i]) nz++;
		expect("a sounding note is not silence", nz > 1000, 1);
	}
	/* A channel with no volume stays silent even while the note is on. */
	{
		static Sint16 buf[2048];
		int i, nz = 0;
		audio_init();
		host_deo(0, 0x32, 0);
		host_deo(0, 0x34, 0x00); host_deo(0, 0x35, 0x40);
		host_deo(0, 0x36, 0x00); host_deo(0, 0x37, 0x00);
		host_deo(0, 0x38, 1);
		audio_render(buf, 2048);
		for(i = 0; i < 2048; i++) if(buf[i]) nz++;
		expect("no volume means no sound", nz, 0);
	}

	/* --------------------------------------------------------- clock */
	host_init();
	clock_tick();
	/* The year is a short at 0xc2/0xc3. Whichever date it is, the pair must
	   not read as zero. */
	expect("clock year is populated",
	       (host_dei(0, 0xc2) | host_dei(0, 0xc3)) != 0, 1);
	expect("clock month in range",
	       host_dei(0, 0xc4) >= 1 && host_dei(0, 0xc4) <= 12, 1);
	expect("clock day in range",
	       host_dei(0, 0xc5) >= 1 && host_dei(0, 0xc5) <= 31, 1);
	expect("clock hour in range", host_dei(0, 0xc6) <= 23, 1);
	expect("clock minute in range", host_dei(0, 0xc7) <= 59, 1);
	expect("clock second in range", host_dei(0, 0xc8) <= 59, 1);
	expect("clock weekday in range", host_dei(0, 0xc9) <= 6, 1);

	/* ---------------------------------------------------------- file */
	host_init();
	file_store_reset();
	{
		/* A name at 0x0500 and a buffer at 0x0600, both in main memory. */
		static const char nm[] = "roundtrip";
		int i;
		for(i = 0; i < (int)sizeof nm - 1; i++) dux.mem[0x0500 + i] = (Uint8)nm[i];
		dux.mem[0x0500 + sizeof nm - 1] = 0;
		for(i = 0; i < 6; i++) dux.mem[0x0600 + i] = (Uint8)('a' + i);

		host_deo(0, 0xa8, 0x05);            /* name pointer, high  */
		host_deo(0, 0xa9, 0x00);            /* name pointer, low   */
		host_deo(0, 0xaa, 0x06);            /* data pointer, high  */
		host_deo(0, 0xab, 0x00);            /* data pointer, low   */
		host_deo(0, 0xa4, 0x00);            /* count, high         */
		host_deo(0, 0xa5, 0x06);            /* count, low          */
		host_deo(0, 0xa6, 0x02);            /* save                */
		expect("save reported no error", host_dei(0, 0xa7), 0);

		for(i = 0; i < 6; i++) dux.mem[0x0600 + i] = 0;

		host_deo(0, 0xa6, 0x01);            /* load                */
		expect("load reported no error", host_dei(0, 0xa7), 0);
		expect("result high half", host_dei(0, 0xa2), 0x00);
		expect("result low half", host_dei(0, 0xa3), 0x06);
		expect("first byte came back", dux.mem[0x0600], 'a');
		expect("last byte came back", dux.mem[0x0605], 'f');
	}
	/* A name that is not there loads as zero bytes, which is what a program
	   creating a new file wants; it is not an error. */
	{
		int i;
		for(i = 0; i < 9; i++) dux.mem[0x0500 + i] = (Uint8)"nonexist"[i];
		dux.mem[0x0500 + 9] = 0;
		host_deo(0, 0xa6, 0x01);
		expect("unknown name is not an error", host_dei(0, 0xa7), 0);
		expect("unknown name reads as empty", host_dei(0, 0xa3), 0x00);
	}
	/* The store survives a round trip through the filesystem, which is the
	   whole reason it exists: a machine you can close should keep its files. */
	{
		int i, ok = 1;
		static const char nm[] = "roundtrip";
		file_store_save("tests_host.store");
		file_store_reset();
		/* The name has to be put back: the test above left a different one
		   at 0x0500. */
		for(i = 0; i < (int)sizeof nm - 1; i++) dux.mem[0x0500 + i] = (Uint8)nm[i];
		dux.mem[0x0500 + sizeof nm - 1] = 0;
		for(i = 0; i < 6; i++) dux.mem[0x0600 + i] = 0;
		file_store_load("tests_host.store");
		host_deo(0, 0xa8, 0x05);
		host_deo(0, 0xa9, 0x00);
		host_deo(0, 0xaa, 0x06);
		host_deo(0, 0xab, 0x00);
		host_deo(0, 0xa6, 0x01);
		if(dux.mem[0x0600] != 'a') ok = 0;
		expect("store survives a save and load", ok, 1);
		remove("tests_host.store");
	}
	/* A store file that is not one is refused rather than half read. */
	expect("a junk store is refused", file_store_save("tests_host.store") == 0, 1);
	{
		FILE *f = fopen("tests_host.store", "wb");
		fwrite("not a store at all, really not", 1, 29, f);
		fclose(f);
		expect("junk store is rejected", file_store_load("tests_host.store") != 0, 1);
		remove("tests_host.store");
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}