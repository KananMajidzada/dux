/* clock.h - the Datetime device.
 *
 * Read-only wall clock. A program that wants to know what time it is has no
 * other way to find out, and a machine that cannot tell time cannot keep a
 * diary. The values are sampled once per frame, so every field read during one
 * frame agrees with every other.
 *
 * The port map uses a device at 0xc0. Host-dependent by nature: this is the
 * one device that means something different on different machines, and a
 * program that cares should read the year and notice when it wraps.
 */

#ifndef CLOCK_H
#define CLOCK_H

#include "dux.h"

/* Sample the host clock into the ports. Call once per frame. */
void clock_tick(void);

Uint8 clock_read(Uint8 addr);

#endif /* CLOCK_H */