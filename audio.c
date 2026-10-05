/* audio.c - the Audio device and its mixer.
 *
 * Port map, device 0x30:
 *
 *   0x30/0x31  vector
 *   0x32       channel      which channel the other ports address, 0..3
 *   0x33       waveform     0 square, 1 triangle, 2 saw, 3 noise
 *   0x34/0x35  period       samples per cycle, so pitch falls as it rises
 *   0x36/0x37  volume       0..0xffff
 *   0x38       note         any write starts the note, 0 stops it
 *   0x39       reserved
 *   0x3a/0x3b  beat         attack time in milliseconds; 0 means none
 *   0x3c/0x3d  envelope     the current level, read only
 *   0x3e       phase        write restarts the envelope and phase
 *
 * A 16-bit port is two consecutive 8-bit ports with the high byte at the lower
 * address, which is the order the core uses for every device pair: a DEO2 to
 * port $34 writes the high byte at $34 and the low byte at $35. Pairs are given
 * even bases so the halves are unambiguous, and each half is handled explicitly
 * below rather than by testing the address's parity.
 */

#include "audio.h"

#include <string.h>

/* The envelope moves once per millisecond. Tying it to the 60Hz frame rate
   instead would mean a note changed by a game could never be heard: the
   attack would still be at zero when the next frame replaced it. */
#define ENV_TICK        (AUDIO_RATE / 1000)

/* How long the tail rings after a note is released, in milliseconds. */
#define RELEASE_MS      20

typedef struct {
	Uint32 phase;      /* 16.16 fixed-point accumulator */
	Uint32 noise;      /* LFSR state, for the noise waveform */
	Uint16 period;
	Uint16 volume;
	Uint16 beat;
	Uint16 env;
	Uint8  waveform;
	Uint8  on;
} Channel;

static Channel chan[AUDIO_CHANNELS];
static int     sel;          /* the channel the ports address */
static int     frames_left;  /* frames until the next envelope step */

/* The vector pair, held in the shadow memory like every other device's. */

void audio_init(void) {
	memset(chan, 0, sizeof chan);
	chan[0].period = chan[1].period = 256;
	chan[2].period = chan[3].period = 256;
	sel = 0;
	frames_left = ENV_TICK;
	dux.mem[IO_BASE + 0x30] = 0;
	dux.mem[IO_BASE + 0x31] = 0;
}

Uint16 audio_vector(void) {
	return (Uint16)((dux.mem[IO_BASE + 0x30] << 8) |
	                dux.mem[IO_BASE + 0x31]);
}

/* Guard the index: a program may write any byte to the channel port. */
static Channel *cur(void) {
	return &chan[sel & (AUDIO_CHANNELS - 1)];
}

/* Replace the high or low half of a short. Spelling out which is which, rather
   than inferring it from the port's parity, keeps this correct for any base
   and impossible to get backwards by eye. */
static Uint16 set_hi(Uint16 v, Uint8 val) { return (Uint16)((v & 0x00ff) | ((Uint16)val << 8)); }
static Uint16 set_lo(Uint16 v, Uint8 val) { return (Uint16)((v & 0xff00) | val); }

Uint8 audio_read(Uint8 addr) {
	Channel *c = cur();
	switch(addr) {
	case 0x32: return (Uint8)sel;
	case 0x33: return c->waveform;
	case 0x34: return (Uint8)(c->period >> 8);
	case 0x35: return (Uint8)c->period;
	case 0x36: return (Uint8)(c->volume >> 8);
	case 0x37: return (Uint8)c->volume;
	case 0x38: return c->on;
	case 0x3a: return (Uint8)(c->beat >> 8);
	case 0x3b: return (Uint8)c->beat;
	case 0x3c: return (Uint8)(c->env >> 8);
	case 0x3d: return (Uint8)c->env;
	case 0x3e: return (Uint8)(c->phase >> 24);
	default:   return dux.mem[IO_BASE + addr];
	}
}

void audio_write(Uint8 addr, Uint8 val) {
	Channel *c = cur();

	switch(addr) {
	case 0x30: case 0x31:                       /* vector */
		dux.mem[IO_BASE + addr] = val;
		return;

	case 0x32: sel = val & (AUDIO_CHANNELS - 1); c = cur(); return;

	case 0x33: c->waveform = val & 0x03; return;

	case 0x34: c->period = set_hi(c->period, val); return;
	case 0x35: c->period = set_lo(c->period, val); return;

	case 0x36: c->volume = set_hi(c->volume, val); return;
	case 0x37: c->volume = set_lo(c->volume, val); return;

	case 0x38:
		/* Any write to note is the attack. Zero is the release, which is
		   how a program stops a note without a separate "off" command. */
		if(val) {
			c->on = 1;
			c->env = 0;
			c->phase = 0;
			c->noise = 0x7fffffff;
		} else {
			c->on = 0;
		}
		return;

	case 0x3a: c->beat = set_hi(c->beat, val); return;
	case 0x3b: c->beat = set_lo(c->beat, val); return;

	case 0x3c: case 0x3d:           /* the envelope is read only */
		return;

	case 0x3e:                      /* restart from the top */
		c->phase = 0;
		c->env = c->volume;
		c->noise = 0x7fffffff;
		return;

	default:
		dux.mem[IO_BASE + addr] = val;
		return;
	}
}

int audio_active(void) {
	int i;
	for(i = 0; i < AUDIO_CHANNELS; i++) {
		if(chan[i].on && chan[i].env) return 1;
	}
	return 0;
}

/* Advance one channel's envelope by one tick.
 *
 * beat is the attack time in milliseconds, so a program chooses how quickly a
 * note reaches full volume rather than having to think in envelope units. A
 * beat of zero means no attack: the note is already at full volume, which is
 * what a sustained tone wants. */
static void step_env(Channel *c) {
	if(c->on) {
		Uint32 inc;
		if(c->beat == 0) {
			c->env = c->volume;
			return;
		}
		inc = ((Uint32)c->volume + c->beat - 1) / c->beat;
		c->env = (Uint16)(((Uint32)c->env + inc > c->volume)
		                  ? c->volume : c->env + inc);
	} else {
		Uint32 dec = ((Uint32)c->volume + RELEASE_MS - 1) / RELEASE_MS;
		if(dec == 0) dec = 1;
		c->env = (Uint16)(c->env <= dec ? 0 : c->env - dec);
	}
}

/* One sample of a waveform, as -128..127 from the top byte of the phase. */
static Sint8 wave(Channel *c, int t) {
	switch(c->waveform) {
	case WAV_TRIANGLE:
		return (Sint8)(t < 0x80 ? (t * 2) : (255 - t) * 2 - 256);
	case WAV_SAW:
		return (Sint8)(t * 2 - 255);
	case WAV_NOISE: {
		/* A 32-bit maximal-length LFSR. Cheap, and the spectrum is even
		   enough for percussion. */
		Uint32 x = c->noise;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		c->noise = x;
		return (Sint8)((x >> 25) & 0xff);
	}
	case WAV_SQUARE:
	default:
		return (Sint8)(t < 0x80 ? 127 : -128);
	}
}

void audio_render(Sint16 *out, int frames) {
	int i, ch;
	Uint32 inc[AUDIO_CHANNELS];

	for(i = 0; i < AUDIO_CHANNELS; i++) inc[i] = 0;

	for(i = 0; i < frames; i++) {
		long mix = 0;

		if(--frames_left <= 0) {
			int k;
			for(k = 0; k < AUDIO_CHANNELS; k++) step_env(&chan[k]);
			frames_left = ENV_TICK;
		}

		for(ch = 0; ch < AUDIO_CHANNELS; ch++) {
			Channel *c = &chan[ch];
			if(!c->env || !c->period) continue;
			if(inc[ch] == 0) inc[ch] = (Uint32)(((Uint32)AUDIO_RATE << 16) / c->period);
			c->phase += inc[ch];
			mix += ((long)wave(c, (int)(c->phase >> 24)) * (long)c->env) >> 16;
		}

		/* Each channel contributes at most 128, so four of them reach 512
		   and cannot wrap a 16-bit sample. No normalisation is wanted: it
		   would cost level, and level is what makes a note audible. The
		   clamp is only a guard against a future fifth channel or a
		   waveform that returns something larger than a byte. */
		if(mix >  32767) mix =  32767;
		if(mix < -32768) mix = -32768;
		out[i] = (Sint16)mix;
	}
}