/* dux.c - 8-bit stack machine, CLI host
 *
 * A headless driver for the Dux core: loads a ROM, runs it, and can
 * disassemble, trace, or dump the framebuffer.
 *
 * The core in dux.c has no dependencies. Everything platform-specific
 * lives here, so the core still links into a freestanding target unchanged.
 */

#include "dux.h"
#include "host.h"
#include "audio.h"
#include "clock.h"
#include "file.h"
#include "screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ opcode names */

static const char *op_names[32] = {
	"BRK", "INC", "POP", "NIP", "SWP", "ROT", "DUP", "OVR",
	"EQU", "NEQ", "GTH", "LTH", "JMP", "JCN", "JSR", "STH",
	"LDZ", "STZ", "LDR", "STR", "LDA", "STA", "DEI", "DEO",
	"ADD", "SUB", "MUL", "DIV", "AND", "ORA", "EOR", "SFT"
};

/* Opcode 0x00 is contextual: the mode bits alone decide what it means. */
static void decode(Uint8 b, char *name, size_t n) {
	switch(b) {
	case OP_JCI: strncpy(name, "JCI", n); break;
	case OP_JMI: strncpy(name, "JMI", n); break;
	case OP_JSI: strncpy(name, "JSI", n); break;
	case OP_LIT: strncpy(name, "LIT", n); break;
	case OP_LIT2: strncpy(name, "LIT2", n); break;
	case OP_LITR: strncpy(name, "LITr", n); break;
	case OP_LIT2R: strncpy(name, "LIT2r", n); break;
	default: strncpy(name, op_names[b & OPCODE_MASK], n); break;
	}
	name[n - 1] = 0;
}

/* Assemble the mode suffix, e.g. "ADD2kr". The contextual opcodes take their
   meaning FROM the mode bits, so they never carry a suffix: 0x80 prints as
   "LIT", not "LITk". */
static void suffix(Uint8 b, char *out) {
	int i = 0;
	if((b & OPCODE_MASK) == BRK) { out[0] = 0; return; }
	if(b & MODE_SHORT) out[i++] = '2';
	if(b & MODE_RETURN) out[i++] = 'r';
	if(b & MODE_KEEP) out[i++] = 'k';
	out[i] = 0;
}

/* --------------------------------------------------------------- devices
 *
 * The device layer lives in host.c so the headless CLI and the SDL front end
 * behave identically. This file only drives it.
 */

/* ------------------------------------------------------------------- rom */

/* Load a flat ROM image at the reset vector. */
static long load_rom(const char *path) {
	FILE *f;
	long size;
	size_t got;

	f = fopen(path, "rb");
	if(!f) {
		fprintf(stderr, "dux: cannot open %s\n", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if(size > MEM_SIZE - RAM_BASE) {
		fprintf(stderr, "dux: rom is %ld bytes, only %d fit in RAM\n",
		        size, MEM_SIZE - RAM_BASE);
		fclose(f);
		return -1;
	}
	got = fread(dux.mem + RAM_BASE, 1, (size_t)size, f);
	fclose(f);
	return (long)got;
}

/* ----------------------------------------------------------- disassembly */

/* Instruction length in bytes. Only the contextual opcodes carry inline
   operands; every other opcode is a single byte and takes its operands from
   the stack. */
static int instruction_length(Uint8 b) {
	switch(b) {
	case OP_LIT:
	case OP_LITR:
		return 2;
	case OP_LIT2:
	case OP_LIT2R:
	case OP_JMI:
	case OP_JCI:
	case OP_JSI:
		return 3;
	default:
		return 1;
	}
}

/* Print one instruction at pc. Returns its length in bytes. */
static int disasm_one(Uint16 pc) {
	Uint8 b = dux.mem[pc];
	char name[8], sfx[4];
	int len = 1;

	decode(b, name, sizeof name);
	suffix(b, sfx);
	printf("%04x  %02x  %-7s", pc, b, name);
	if(sfx[0]) printf(" %s", sfx);

	switch(b) {
	case OP_LIT:
		printf(" #%02x", dux.mem[(Uint16)(pc + 1)]);
		len = 2;
		break;
	case OP_LITR:
		printf(" #%02x", dux.mem[(Uint16)(pc + 1)]);
		len = 2;
		break;
	case OP_LIT2:
		printf(" #%02x%02x", dux.mem[(Uint16)(pc + 1)], dux.mem[(Uint16)(pc + 2)]);
		len = 3;
		break;
	case OP_LIT2R:
		printf(" #%02x%02x", dux.mem[(Uint16)(pc + 1)], dux.mem[(Uint16)(pc + 2)]);
		len = 3;
		break;
	case OP_JMI:
	case OP_JCI:
	case OP_JSI:
		printf(" %04x", (Uint16)(dux.mem[(Uint16)(pc + 1)] << 8 |
		                         dux.mem[(Uint16)(pc + 2)]));
		len = 3;
		break;
	default:
		break;
	}
	printf("\n");
	return len;
}

static void disasm(Uint16 pc, int count) {
	int i, n;
	for(i = 0; i < count; i++) {
		n = disasm_one(pc);
		if(n < 1) break;
		pc = (Uint16)(pc + n);
	}
}

/* ------------------------------------------------------------------- ppm */

/* Write the framebuffer as a binary PPM. Four palette entries, set with the
   System device's r/g/b shorts, are expanded into RGB. */
static void dump_ppm(const char *path) {
	FILE *f;
	int x, y;
	Uint8 pal[4][3];
	Uint8 shift, idx;
	Uint16 addr;
	static Uint8 row[FB_WIDTH * 3];

	f = fopen(path, "wb");
	if(!f) {
		fprintf(stderr, "dux: cannot write %s\n", path);
		return;
	}

	/* Ask the screen device for the live palette, so a program that changed
	   its colours shows up in the dump. */
	{
		int c, k;
		for(c = 0; c < 4; c++) {
			const Uint8 *rgb = screen_palette(c);
			for(k = 0; k < 3; k++) pal[c][k] = rgb[k];
		}
	}

	fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
	for(y = 0; y < FB_HEIGHT; y++) {
		for(x = 0; x < FB_WIDTH; x++) {
			addr  = (Uint16)(FB_BASE + (y * FB_WIDTH + x) / 4);
			shift = (Uint8)((x & 3) * 2);
			idx   = (Uint8)((dux.mem[addr] >> shift) & 0x3);
			row[x * 3]     = pal[idx][0];
			row[x * 3 + 1] = pal[idx][1];
			row[x * 3 + 2] = pal[idx][2];
		}
		fwrite(row, 1, sizeof row, f);
	}
	fclose(f);
	fprintf(stderr, "dux: wrote %s (%dx%d)\n", path, FB_WIDTH, FB_HEIGHT);
}

/* ------------------------------------------------------------------ trace */

static void trace_stack(void) {
	int i;
	Uint8 n = dux.wstp > 4 ? 4 : dux.wstp;
	fprintf(stderr, "    wst[%d]:", dux.wstp);
	for(i = 0; i < n; i++)
		fprintf(stderr, " %02x", dux.wst[(Uint8)(dux.wstp - 1 - i)]);
	fprintf(stderr, "   rst[%d]:", dux.rstp);
	for(i = 0; i < n; i++)
		fprintf(stderr, " %02x", dux.rst[(Uint8)(dux.rstp - 1 - i)]);
	fprintf(stderr, "\n");
}

/* -------------------------------------------------------------------- audio
 *
 * Render the audio device to a WAV file. Since the synthesis lives in audio.c
 * and the front end only decides how many samples to ask for, what this writes
 * is what a speaker would produce. That makes audio testable with no sound
 * card and no listener.
 */

#define WAV_CHUNK 4096

static void put32(FILE *f, unsigned long v) {
	fputc((int)(v & 0xff), f);
	fputc((int)((v >> 8) & 0xff), f);
	fputc((int)((v >> 16) & 0xff), f);
	fputc((int)((v >> 24) & 0xff), f);
}

static void put16(FILE *f, unsigned v) {
	fputc((int)(v & 0xff), f);
	fputc((int)((v >> 8) & 0xff), f);
}

static void wav_header(FILE *f, unsigned long frames) {
	unsigned long bytes = frames * 2;
	fwrite("RIFF", 1, 4, f);
	put32(f, 36 + bytes);
	fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f);
	put32(f, 16);
	put16(f, 1);                                   /* PCM */
	put16(f, 1);                                   /* mono */
	put32(f, AUDIO_SAMPLE_RATE);
	put32(f, (unsigned long)AUDIO_SAMPLE_RATE * 2);/* bytes per second */
	put16(f, 2);                                   /* block align */
	put16(f, 16);                                  /* bits per sample */
	fwrite("data", 1, 4, f);
	put32(f, bytes);
}

/* Write `frames` of audio. Returns 0 on success. */
static int dump_wav(const char *path, long frames) {
	static Sint16 buf[WAV_CHUNK];
	FILE *f;
	long done = 0;

	f = fopen(path, "wb");
	if(!f) {
		fprintf(stderr, "dux: cannot write %s\n", path);
		return 1;
	}
	wav_header(f, (unsigned long)frames);
	while(done < frames) {
		long left = frames - done;
		int n = (int)(left < WAV_CHUNK ? left : WAV_CHUNK);
		int i;
		audio_render(buf, n);
		for(i = 0; i < n; i++)
			put16(f, (unsigned)(Uint16)buf[i]);
		done += n;
	}
	if(fclose(f) != 0) {
		fprintf(stderr, "dux: error writing %s\n", path);
		return 1;
	}
	fprintf(stderr, "dux: wrote %s (%ld frames)\n", path, frames);
	return 0;
}

/* ------------------------------------------------------------------ usage */

static void usage(void) {
	/* Kept as separate calls: one string literal that long is more than
	   C90 guarantees a compiler will accept. */
	fprintf(stderr, "dux - an 8-bit stack machine\n\n");
	fprintf(stderr, "usage: dux [options] <rom>\n\n");
	fprintf(stderr, "  -c, --cycles N     step budget per vector (default 100000)\n");
	fprintf(stderr, "  -d, --disasm [N]   print N instructions, then exit\n");
	fprintf(stderr, "  -f, --frames N     run N frames\n");
	fprintf(stderr, "  -p, --ppm FILE     write the framebuffer to FILE after running\n");
	fprintf(stderr, "  -w, --wav FILE     render the audio device to a WAV file\n");
	fprintf(stderr, "  -s, --store FILE   load and save the File device store\n");
	fprintf(stderr, "  -k, --key N        hold key code N down, for input tests\n");
	fprintf(stderr, "  -m, --mouse LIST   mouse at X,Y or a ;-separated path;\n"
	                "                 a third field on a point is the buttons held\n");
	fprintf(stderr, "  -b, --button MASK  hold mouse buttons, bit 0 left, 1 right\n");
	fprintf(stderr, "  -t, --trace        trace execution until it hits BRK\n");
	fprintf(stderr, "  -h, --help         this message\n\n");
	fprintf(stderr, "memory map:  0000-00ff zero page, 0100-01ff devices,\n");
	fprintf(stderr, "             0200-7fff ram, 8000-ffff video\n");
}

int main(int argc, char **argv) {
	const char *rom = 0, *ppm = 0, *wav = 0, *store = 0;
	int hold = -1;
	int mouse_x = 0, mouse_y = 0, have_mouse = 0, at_x = 0, at_y = 0;
	int buttons = 0;
	int step = 0, nsteps = 0;
	int *px_at = 0, *py_at = 0, *pb_at = 0;
	long cycles = 100000;
	int trace = 0, disasm_n = 0, frames = 0, verbose = 0;
	int i;
	long n;

	for(i = 1; i < argc; i++) {
		const char *a = argv[i];
		if(a[0] != '-' || a[1] == 0) { rom = a; continue; }
		if(!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
		if(!strcmp(a, "-t") || !strcmp(a, "--trace")) { trace = 1; continue; }
		if(!strcmp(a, "-f") || !strcmp(a, "--frames")) { frames = 1; verbose = 1; continue; }
		if((!strcmp(a, "-n") || !strcmp(a, "--frame-count")) && i + 1 < argc) {
			frames = (int)strtol(argv[++i], 0, 0); continue;
		}
		if((!strcmp(a, "-c") || !strcmp(a, "--cycles")) && i + 1 < argc) {
			cycles = strtol(argv[++i], 0, 0); continue;
		}
		if((!strcmp(a, "-d") || !strcmp(a, "--disasm")) && i + 1 < argc &&
		   argv[i + 1][0] != '-') {
			disasm_n = (int)strtol(argv[++i], 0, 0);
			continue;
		}
		if(!strcmp(a, "-d") || !strcmp(a, "--disasm")) {
			disasm_n = 20; continue;
		}
		if((!strcmp(a, "-p") || !strcmp(a, "--ppm")) && i + 1 < argc) {
			ppm = argv[++i]; continue;
		}
		if((!strcmp(a, "-w") || !strcmp(a, "--wav")) && i + 1 < argc) {
			wav = argv[++i]; continue;
		}
		if((!strcmp(a, "-s") || !strcmp(a, "--store")) && i + 1 < argc) {
			store = argv[++i]; continue;
		}
		if((!strcmp(a, "-k") || !strcmp(a, "--key")) && i + 1 < argc) {
			hold = (int)strtol(argv[++i], 0, 0); continue;
		}
		if((!strcmp(a, "-m") || !strcmp(a, "--mouse")) && i + 1 < argc) {
			/* One point, or a path of them: 10,20 or 10,20;30,40;50,60.
			   A path is what makes a drag testable, because a cursor held
			   still over one pixel proves nothing about a trail.

			   A point may carry a third field: the buttons held while it is
			   current, so "10,20,1;10,20,0" is a click in one place. -b holds
			   a single mask for the whole run and cannot express a press and
			   then a release, so a game acting on the pressing edge would see
			   exactly one press, on frame one, and no test could ever click
			   twice. Position was the only thing -m could vary, which is why a
			   game with buttons on it could not be driven at all. The third
			   field defaults to nothing, so every path that already worked
			   still reads the same. */
			const char *p = argv[++i];
			int cap = 0;
			free(px_at); px_at = 0; py_at = 0; free(pb_at); pb_at = 0;
			nsteps = 0;
			while(*p) {
				int x, y, b = 0;
				if(sscanf(p, "%d,%d,%d", &x, &y, &b) < 2) break;
				if(nsteps == cap) {
					int *nx, *ny, *nb;
					cap = cap ? cap * 2 : 16;
					nx = (int *)realloc(px_at, (size_t)cap * sizeof(int));
					if(!nx) { free(nx); return 1; }
					px_at = nx;
					ny = (int *)realloc(py_at, (size_t)cap * sizeof(int));
					if(!ny) { free(ny); return 1; }
					py_at = ny;
					nb = (int *)realloc(pb_at, (size_t)cap * sizeof(int));
					if(!nb) { free(nb); return 1; }
					pb_at = nb;
				}
				px_at[nsteps] = x;
				py_at[nsteps] = y;
				pb_at[nsteps] = b;
				nsteps++;
				while(*p && *p != ';') p++;
				if(*p == ';') p++;
			}
			if(nsteps) {
				have_mouse = 1;
				mouse_x = px_at[0];
				mouse_y = py_at[0];
				buttons = pb_at[0];
				step = 0;
			}
			continue;
		}
		if((!strcmp(a, "-b") || !strcmp(a, "--button")) && i + 1 < argc) {
			buttons = (int)strtol(argv[++i], 0, 0); continue;
		}
		fprintf(stderr, "dux: unknown option %s\n", a);
		return 1;
	}

	if(!rom) { usage(); return 1; }

	host_init();

	/* The store is loaded before the ROM runs, so a program sees whatever the
	   last session left behind, and flushed after, so what it wrote persists. */
	if(store && file_store_load(store) != 0)
		fprintf(stderr, "dux: ignoring unreadable store %s\n", store);

	n = load_rom(rom);
	if(n < 0) return 1;
	fprintf(stderr, "dux: loaded %ld bytes at %04x\n", n, RAM_BASE);

	if(disasm_n) {
		disasm((Uint16)RESET_VECTOR, disasm_n);
		return 0;
	}

	if(frames > 0) {
		/* Frame loop. Each iteration runs every non-zero device vector to
		   BRK, then advances the frame counter. Non-interruptible: the
		   host decides when a vector starts, never the CPU.

		   The reset vector runs first, since that is where a program
		   installs its vectors. */
		int  f;
		/* Power-on: run the reset vector, which installs device vectors. */
		host_reset();

		/* A held key has to be re-armed every frame. The Controller delivers
		   one event per press, so feeding it once would look like a single
		   tap no matter how many frames run. */
		if(hold >= 0) host_key_down((Uint8)hold);

		for(f = 0; f < frames && !host_quit_requested(); f++) {
			if(verbose) fprintf(stderr, "dux: frame %d\n", f);
			if(hold >= 0) host_key_down((Uint8)hold);
			/* The mouse arrives as a delta, so an absolute test position
			   means feeding the difference from where it was last put. Zero
			   deltas would be swallowed, which is right for a real mouse
			   sitting still and wrong for a test that wants it visible on
			   frame one, so the position is nudged into place first. */
			if(have_mouse) {
				/* A one-point path stays put; a longer one advances a
				   point per frame and then rests at the last, so a
				   drag ends where the path said it should. */
				if(nsteps > 1) {
					if(step < nsteps - 1) step++;
					mouse_x = px_at[step];
					mouse_y = py_at[step];
				}
				host_mouse_move(mouse_x - at_x, mouse_y - at_y);
				at_x = mouse_x;
				at_y = mouse_y;
				host_mouse_button(1, have_mouse ? pb_at[step] : buttons);
			}
			host_frame();
		}
		if(ppm) dump_ppm(ppm);
		if(wav && dump_wav(wav, (long)frames * (AUDIO_SAMPLE_RATE / 60)))
			return 1;
		if(store && file_store_save(store) != 0) {
			fprintf(stderr, "dux: cannot write store %s\n", store);
			return 1;
		}
		free(px_at);
		free(py_at);
		return 0;
	}

	if(trace) {
		/* One instruction at a time. dux_eval publishes the PC it stopped at,
		   so stepping follows jumps and calls instead of walking addresses in
		   order, which would miss every branch in the program. */
		long steps = 0;
		clock_tick();
		for(;;) {
			Uint16 at = dux.pc;
			Uint8 b = dux.mem[at];
			char name[8], sfx[4];
			int len;

			if(at != (Uint16)RESET_VECTOR && b == BRK && steps > 0) break;
			if(steps++ > cycles) {
				fprintf(stderr, "dux: trace step limit at %04x\n", at);
				break;
			}
			decode(b, name, sizeof name);
			suffix(b, sfx);
			fprintf(stderr, "%04x  %-6s", at, name);
			if(sfx[0]) fprintf(stderr, "%-3s", sfx);
			len = instruction_length(b);
			if(len > 1) {
				int i;
				fprintf(stderr, " #");
				for(i = 1; i < len; i++) fprintf(stderr, "%02x", dux.mem[(Uint16)(at + i)]);
			}
			trace_stack();
			dux_eval(at, 1);        /* executes exactly one instruction */
			if(host_quit_requested()) break;
		}
	} else {
		int status;
		/* This path does not go through host_reset, so the wall clock has to
		   be sampled here or a program asking the time reads zeroes. */
		clock_tick();
		status = dux_eval((Uint16)RESET_VECTOR, (unsigned long)cycles);
		if(status) fprintf(stderr, "dux: step budget exhausted at %04x\n", dux.pc);
	}

	if(ppm) dump_ppm(ppm);
	return 0;
}