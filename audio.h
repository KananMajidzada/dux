/* audio.h - the Audio device, and the mixer that feeds it.
 *
 * Four channels of 8-bit style synthesis: square, triangle, sawtooth and
 * noise. Each is a phase accumulator plus an envelope, which is more than a
 * machine this size strictly needs and less than a sound card.
 *
 * The device writes land here; the samples come out through audio_render().
 * That split is deliberate: the SDL front end calls render from its audio
 * callback, and the headless CLI calls it once per frame to write a WAV file.
 * The synthesis is identical either way, so a headless render is a faithful
 * prediction of what a speaker would produce.
 *
 * No SDL, no dependencies beyond dux.h.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include "dux.h"

#define AUDIO_CHANNELS  4
#define AUDIO_RATE      44100

/* Sample rate, for a front end that has to open its device with one. */
#define AUDIO_SAMPLE_RATE AUDIO_RATE

/* Waveform selectors, as written to Audio/waveform. */
#define WAV_SQUARE   0
#define WAV_TRIANGLE 1
#define WAV_SAW      2
#define WAV_NOISE    3

void audio_init(void);

Uint8 audio_read(Uint8 addr);
void  audio_write(Uint8 addr, Uint8 val);

/* Mix `frames` samples into out, which must hold that many Sint16. */
void  audio_render(Sint16 *out, int frames);

/* Non-zero while any channel is sounding, so a front end knows whether it
   needs an audio device at all. */
int   audio_active(void);

/* The device's vector, read from the low and high ports. */
Uint16 audio_vector(void);

#endif /* AUDIO_H */