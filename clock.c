/* clock.c - the Datetime device.
 *
 * Port map, device 0xc0:
 *
 *   0xc0/0xc1  vector
 *   0xc2/0xc3  year      full year, so 2026 rather than 26
 *   0xc4       month     1..12
 *   0xc5       day       1..31
 *   0xc6       hour      0..23
 *   0xc7       minute    0..59
 *   0xc8       second    0..59
 *   0xc9       weekday   0 Sunday .. 6 Saturday
 *   0xca       yearday   1..366
 *
 * Everything is read-only. A clock that programs can set is a clock they can
 * forget to set.
 */

#include "clock.h"

#include <time.h>
#include <string.h>
#include <stdlib.h>

static Uint8 port[16];

void clock_tick(void) {
	static int pinned = -1;
	static time_t fixed;
	time_t now;
	struct tm *t;
	int year;

	memset(port, 0, sizeof port);

	/* A clock reads live time, which makes it the one ROM on this project
	 * whose picture cannot be checked against a recorded pixel count: the
	 * digits change with the minute, so the same ROM renders differently
	 * every hour. DUX_TIME pins the device to a given epoch so a render can
	 * be compared against a number written down in advance, which is how
	 * every other ROM here is verified. Unset, the behaviour is unchanged.
	 */
	if(pinned < 0) {
		const char *e = getenv("DUX_TIME");
		pinned = e ? 1 : 0;
		if(e) fixed = (time_t)strtol(e, NULL, 10);
	}

	now = pinned ? fixed : time(0);
	t = localtime(&now);
	if(!t) return;

	year = t->tm_year + 1900;
	if(year < 0 || year > 0xffff) return;

	/* Offsets are relative to the device base at 0xc0, and a short holds
	   its high byte at the base, like every other device pair on the page. */
	port[0x02] = (Uint8)(year >> 8);
	port[0x03] = (Uint8)(year & 0xff);
	port[0x04] = (Uint8)(t->tm_mon + 1);
	port[0x05] = (Uint8)(t->tm_mday);
	port[0x06] = (Uint8)(t->tm_hour);
	port[0x07] = (Uint8)(t->tm_min);
	port[0x08] = (Uint8)(t->tm_sec);
	port[0x09] = (Uint8)(t->tm_wday);
	port[0x0a] = (Uint8)((t->tm_yday + 1) & 0xff);
}

Uint8 clock_read(Uint8 addr) {
	/* addr is the full device address, 0xc0..0xcf; the fields are stored
	   relative to the device base, so strip the base before indexing. */
	addr = (Uint8)(addr & 0x0f);
	if(addr < sizeof port) return port[addr];
	return dux.mem[IO_BASE + addr];
}