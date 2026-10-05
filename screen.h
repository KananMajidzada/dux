/* screen.h - the Screen device. */

#ifndef SCREEN_H
#define SCREEN_H

#include "dux.h"

void screen_init(void);

Uint16 screen_width(void);
Uint16 screen_height(void);

Uint8 screen_read(Uint8 addr);
void  screen_write(Uint8 addr, Uint8 val);

void screen_plot(int x, int y, Uint8 index);
Uint8 screen_get(int x, int y);
void screen_rect(int x, int y, int w, int h, Uint8 index);

void screen_cursor(Uint16 *x, Uint16 *y);

/* The vector address the program installed, or 0 if none. */
Uint16 screen_vector(void);

void screen_set_colour(int index, Uint8 r, Uint8 g, Uint8 b);
const Uint8 *screen_palette(int index);

#endif /* SCREEN_H */