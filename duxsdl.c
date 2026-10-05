/* duxsdl.c - the SDL2 front end for Dux.
 *
 * Opens a window, expands the 320x200 2bpp framebuffer into pixels once per
 * frame, and feeds keyboard and mouse events into the shared device layer.
 * The core and the device layer are unchanged; this file is windowing and
 * event translation only.
 *
 *   ./duxsdl [-s N] [-m] [-F N] [-S FILE] rom
 *     -s N       integer scale factor (default 3)
 *     -m         mute: do not try to open audio
 *     -F N       run N frames then exit, for testing
 *     -S FILE    after the last frame, write a PPM of the framebuffer
 *
 * The -F and -S options make the front end usable without a display, which is
 * how it gets tested.
 */

#include "dux.h"
#include "host.h"
#include "audio.h"
#include "screen.h"

#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME_MS  (1000 / 60)

static int scale = 3;

/* ------------------------------------------------------------- pixels
 *
 * The framebuffer is 2 bits per pixel, four pixels to the byte, high pixel
 * first within each nibble-pair. Expanding it every frame is 64,000 pixels,
 * which is nothing next to the blit.
 */

static Uint32 pixels[FB_WIDTH * FB_HEIGHT];
static Uint8  row[FB_WIDTH * 4];

/* Palette as 32-bit values, refreshed only when the program changes it. */
static Uint32 pal[4];

static void refresh_palette(void) {
	int i;
	for(i = 0; i < 4; i++) {
		Uint8 r, g, b;
		host_colour(i, &r, &g, &b);
		pal[i] = ((Uint32)r << 16) | ((Uint32)g << 8) | b;
	}
}

static void expand_framebuffer(void) {
	const Uint8 *fb = host_framebuffer();
	int x, y;
	for(y = 0; y < FB_HEIGHT; y++) {
		const Uint8 *line = fb + (y * FB_WIDTH) / 4;
		Uint8 *out = row;
		for(x = 0; x < FB_WIDTH; x++) {
			Uint8 byte = line[x >> 2];
			/* The leftmost pixel of a group is the *low* pair, which is the
			   order screen.c plots in: shift (x & 3) * 2. Reading it the
			   other way round mirrors every group of four, which turns a
			   seven-segment digit into scattered pixels. */
			int shift = (x & 3) * 2;
			Uint32 c = pal[(byte >> shift) & 0x3];
			out[0] = (Uint8)(c >> 16);
			out[1] = (Uint8)(c >> 8);
			out[2] = (Uint8)c;
			out[3] = 0xff;
			out += 4;
		}
		memcpy(&pixels[y * FB_WIDTH], row, FB_WIDTH * 4);
	}
}

/* ------------------------------------------------------------- keyboard */

/* Translate an SDL scancode into the host's key code. Printable keys map to
   ASCII so a program can compare against 'a' or '1'. */
static Uint8 key_from_scancode(SDL_Scancode sc) {
	switch(sc) {
	case SDL_SCANCODE_LEFT:     return KEY_LEFT;
	case SDL_SCANCODE_RIGHT:    return KEY_RIGHT;
	case SDL_SCANCODE_UP:       return KEY_UP;
	case SDL_SCANCODE_DOWN:     return KEY_DOWN;
	case SDL_SCANCODE_HOME:     return KEY_HOME;
	case SDL_SCANCODE_END:      return KEY_END;
	case SDL_SCANCODE_PAGEUP:   return KEY_PAGEUP;
	case SDL_SCANCODE_PAGEDOWN: return KEY_PAGEDN;
	case SDL_SCANCODE_ESCAPE:   return KEY_ESC;
	case SDL_SCANCODE_RETURN:
	case SDL_SCANCODE_KP_ENTER: return KEY_ENTER;
	case SDL_SCANCODE_TAB:      return KEY_TAB;
	case SDL_SCANCODE_BACKSPACE:return 0x08;
	case SDL_SCANCODE_DELETE:   return KEY_DELETE;
	case SDL_SCANCODE_F1:       return KEY_F1;
	case SDL_SCANCODE_F2:       return KEY_F2;
	case SDL_SCANCODE_F3:       return KEY_F3;
	case SDL_SCANCODE_F4:       return KEY_F4;
	case SDL_SCANCODE_F5:       return KEY_F5;
	case SDL_SCANCODE_F6:       return KEY_F6;
	case SDL_SCANCODE_F7:       return KEY_F7;
	case SDL_SCANCODE_F8:       return KEY_F8;
	case SDL_SCANCODE_F9:       return KEY_F9;
	case SDL_SCANCODE_F10:      return KEY_F10;
	case SDL_SCANCODE_SPACE:    return ' ';
	default: break;
	}
	/* Letters and digits, ignoring shift: SDL reports the physical key, so
	   'A' and 'a' land on the same code. That suits a game. */
	if(sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
		return (Uint8)('a' + (sc - SDL_SCANCODE_A));
	if(sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
		return (Uint8)('1' + (sc - SDL_SCANCODE_1));
	if(sc == SDL_SCANCODE_0) return '0';
	return 0;
}

/* ------------------------------------------------------------------- audio
 *
 * SDL only has to supply the buffer; the synthesis lives in audio.c, shared
 * with the headless front end. That is the point of keeping it there: a WAV
 * written by `dux -w` and what the speaker plays come from the same code, so
 * listening is optional and testing is not.
 */

static int audio_on = 1;

static void audio_callback(void *ud, Uint8 *stream, int len) {
	(void)ud;
	if(len > 0) audio_render((Sint16 *)stream, len / (int)sizeof(Sint16));
}

/* ------------------------------------------------------------------- main */

static int load_rom(const char *path) {
	FILE *f = fopen(path, "rb");
	long size;
	if(!f) { fprintf(stderr, "duxsdl: cannot open %s\n", path); return 0; }
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(size > MEM_SIZE - RAM_BASE) {
		fprintf(stderr, "duxsdl: rom is %ld bytes, only %d fit\n", size, MEM_SIZE - RAM_BASE);
		fclose(f);
		return 0;
	}
	if(fread(dux.mem + RAM_BASE, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "duxsdl: short read on %s\n", path);
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

int main(int argc, char **argv) {
	const char *rom = 0;
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	SDL_AudioDeviceID aud = 0;
	SDL_AudioSpec want, have;
	int i, running = 1, quit = 0, frame_limit = 0, frames_run = 0;
	const char *shot = 0;
	Uint8 held_buttons = 0;
	/* Last mouse position in framebuffer pixels, so window motion can be
	   converted and still sent to the host as a delta. */
	int at_fx = 0, at_fy = 0;
	Uint32 next_frame;

	for(i = 1; i < argc; i++) {
		if(!strcmp(argv[i], "-m")) { audio_on = 0; continue; }
		if(!strcmp(argv[i], "-F") && i + 1 < argc) {
			frame_limit = atoi(argv[++i]);
			continue;
		}
		if((!strcmp(argv[i], "-S") || !strcmp(argv[i], "--shot")) && i + 1 < argc) {
			shot = argv[++i];
			continue;
		}
		if(!strcmp(argv[i], "-s") && i + 1 < argc) {
			scale = atoi(argv[++i]);
			if(scale < 1) scale = 1;
			continue;
		}
		if(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf(
"duxsdl - the SDL front end for Dux\n"
"\n"
"usage: duxsdl [options] <rom>\n"
"\n"
"  -s, --scale N     window scale, 1..8 (default 2)\n"
"  -F N              run N frames, then exit\n"
"  -S, --shot FILE   write the last frame to FILE as a PPM, then exit\n"
"  -m                mute: run without opening the audio device\n"
"  -h, --help        this message\n"
"\n"
"memory map:  0000-00ff zero page, 0100-01ff devices,\n"
"             0200-7fff ram, 8000-ffff video\n");
			return 0;
		}
		if(argv[i][0] != '-') { rom = argv[i]; continue; }
		fprintf(stderr, "duxsdl: unknown option %s\n", argv[i]);
		return 1;
	}
	if(!rom) {
		fprintf(stderr, "usage: duxsdl [options] rom  (try -h)\n");
		return 1;
	}

	if(SDL_Init(SDL_INIT_VIDEO | (audio_on ? SDL_INIT_AUDIO : 0)) != 0) {
		fprintf(stderr, "duxsdl: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}

	win = SDL_CreateWindow("dux",
	                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                       FB_WIDTH * scale, FB_HEIGHT * scale,
	                       SDL_WINDOW_RESIZABLE);
	if(!win) { fprintf(stderr, "duxsdl: window: %s\n", SDL_GetError()); return 1; }

	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if(!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if(!ren) { fprintf(stderr, "duxsdl: renderer: %s\n", SDL_GetError()); return 1; }

	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
	                        SDL_TEXTUREACCESS_STREAMING, FB_WIDTH, FB_HEIGHT);
	if(!tex) { fprintf(stderr, "duxsdl: texture: %s\n", SDL_GetError()); return 1; }
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	if(audio_on) {
		SDL_zero(want);
		want.freq     = AUDIO_SAMPLE_RATE;
		want.format   = AUDIO_S16SYS;
		want.channels = 1;
		want.samples  = 1024;
		want.callback = audio_callback;
		aud = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if(aud) SDL_PauseAudioDevice(aud, 0);
		else fprintf(stderr, "duxsdl: audio unavailable: %s\n", SDL_GetError());
	}

	host_init();
	if(!load_rom(rom)) return 1;
	refresh_palette();
	host_reset();
	if(host_quit_requested()) return 0;

	next_frame = SDL_GetTicks();

	while(running && !quit) {
		SDL_Event e;
		Uint32 now;

		/* Drain every event first, so a frame sees the final state. */
		while(SDL_PollEvent(&e)) {
			switch(e.type) {
			case SDL_QUIT:
				running = 0;
				break;
			case SDL_KEYDOWN:
				if(e.key.keysym.sym == SDLK_ESCAPE && e.key.keysym.mod & KMOD_CTRL) {
					running = 0;
				} else {
					Uint8 k = key_from_scancode(e.key.keysym.scancode);
					if(k) {
						host_key_down(k);
						if(k >= 0x20 && k < 0x7f) host_text(k);
					}
				}
				break;
			case SDL_KEYUP: {
				Uint8 k = key_from_scancode(e.key.keysym.scancode);
				if(k) host_key_up(k);
				break;
			}
			case SDL_MOUSEMOTION: {
				/* Motion deltas arrive in *window* pixels, and the window
				   is FB_WIDTH*scale wide, so feeding them straight to
				   the host advances the framebuffer cursor scale times
				   too far: it crosses the whole screen in a third of
				   the movement and then sits pinned against the clamp,
				   which reads as a cursor that will not follow the
				   mouse. Converting to framebuffer coordinates first
				   keeps the two in step at any scale.

				   Absolute position divided by scale, rather than the
				   delta divided by scale, because deltas lose the
				   sub-pixel remainder and drift over a long drag.
				   The host takes deltas, so the previous framebuffer
				   position is kept here and the difference sent. */
				int fx = e.motion.x / scale;
				int fy = e.motion.y / scale;
				host_mouse_move(fx - at_fx, fy - at_fy);
				at_fx = fx;
				at_fy = fy;
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP: {
				Uint8 bit = 0;
				if(e.button.button == SDL_BUTTON_LEFT)  bit = MB_LEFT;
				if(e.button.button == SDL_BUTTON_RIGHT) bit = MB_RIGHT;
				if(e.button.button == SDL_BUTTON_MIDDLE) bit = MB_MIDDLE;
				if(e.type == SDL_MOUSEBUTTONDOWN) held_buttons |= bit;
				else                           held_buttons &= (Uint8)~bit;
				host_mouse_button(e.type == SDL_MOUSEBUTTONDOWN, held_buttons);
				break;
			}
			case SDL_MOUSEWHEEL:
				host_scroll(e.wheel.x, e.wheel.y);
				break;
			case SDL_WINDOWEVENT:
				break;
			default:
				break;
			}
			if(!running) break;
		}

		/* Release a button if the window lost focus, otherwise it would stay
		   held forever. */
		if(e.type == SDL_WINDOWEVENT &&
		   e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
			held_buttons = 0;
			host_mouse_button(0, 0);
		}

		if(!host_frame()) quit = 1;
		frames_run++;
		if(frame_limit && frames_run >= frame_limit) quit = 1;

		refresh_palette();
		expand_framebuffer();
		SDL_UpdateTexture(tex, NULL, pixels, FB_WIDTH * 4);
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);

		/* Hold a steady 60Hz. Sleeping the remainder keeps the front end from
		   spinning a core flat, which matters on a machine meant to run on
		   whatever is lying around. */
		now = SDL_GetTicks();
		if(now < next_frame + FRAME_MS) SDL_Delay(next_frame + FRAME_MS - now);
		next_frame += FRAME_MS;
		if(next_frame + FRAME_MS < now) next_frame = now;   /* fell behind */
	}

	if(shot) {
		/* Dump the framebuffer rather than the window, so the capture does
		   not depend on the scale factor or the display mode. */
		FILE *f = fopen(shot, "wb");
		if(f) {
			int x, y;
			fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
			for(y = 0; y < FB_HEIGHT; y++)
				for(x = 0; x < FB_WIDTH; x++)
					fwrite(&pixels[y * FB_WIDTH + x], 1, 3, f);
			fclose(f);
			fprintf(stderr, "duxsdl: wrote %s after %d frames\n",
			        shot, frames_run);
		}
	}

	if(aud) { SDL_CloseAudioDevice(aud); }
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}