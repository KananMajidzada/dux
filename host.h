/* host.h - the device layer, shared by the headless and SDL front ends.
 *
 * The core knows nothing about devices: it calls the two callbacks in
 * dux_attach, and this file decides what they mean. Keeping one device layer
 * means the headless CLI and the SDL front end behave identically, which is
 * what makes the PPM dumps trustworthy.
 *
 * The CPU is non-interruptible. Input arriving mid-vector is queued, and the
 * vector runs once the current one reaches BRK. See host_frame().
 */

#ifndef HOST_H
#define HOST_H

#include "dux.h"

/* ---------------------------------------------------------- input codes
 *
 * Printable keys use ASCII. The few non-printable keys get low codes, so a
 * program can test for them without colliding with text.
 */
#define KEY_TAB     0x09
#define KEY_ENTER   0x0d
#define KEY_ESC     0x1b
#define KEY_LEFT    0x01
#define KEY_RIGHT   0x02
#define KEY_UP      0x03
#define KEY_DOWN    0x04
#define KEY_HOME    0x05
#define KEY_END     0x06
#define KEY_PAGEUP  0x07
#define KEY_PAGEDN  0x08
#define KEY_F1      0x10
#define KEY_F2      0x11
#define KEY_F3      0x12
#define KEY_F4      0x13
#define KEY_F5      0x14
#define KEY_F6      0x15
#define KEY_F7      0x16
#define KEY_F8      0x17
#define KEY_F9      0x18
#define KEY_F10     0x19
#define KEY_DELETE  0x7f

/* Mouse button bits, in the Controller/button and Mouse/state ports. */
#define MB_LEFT     0x01
#define MB_RIGHT    0x02
#define MB_MIDDLE   0x04

/* ---------------------------------------------------------------- lifecycle */

void host_init(void);
void host_reset(void);        /* run the reset vector at 0x0200 */

/* Run one frame: dispatch any queued events to their vectors, then run the
   screen vector if the program installed one. Returns 0 once the program has
   asked to quit. */
int  host_frame(void);

int  host_quit_requested(void);

/* ------------------------------------------------------------- input feed
 *
 * Front ends call these from their event loops. Each queues an event; nothing
 * runs until host_frame().
 */

void host_key_down(Uint8 code);
void host_key_up(Uint8 code);
void host_mouse_move(int dx, int dy);
void host_mouse_button(int down, int buttons);
void host_scroll(int dx, int dy);
void host_text(int ch);          /* printable character, for the console */

/* Consume one queued console character, or -1 if none is waiting. */
int  host_console_read(void);

/* Print a NUL-terminated string from memory, or a 16-bit value in decimal.
   These back the Console/print and Console/number ports. */
void host_print_str(Uint16 addr);
void host_print_num(Uint16 value);

/* Number of console characters waiting. */
int  host_console_pending(void);

/* ------------------------------------------------------------- palette */

/* Expand the four framebuffer colours into 32-bit RGBA. Index by colour. */
void host_colour(int index, Uint8 *r, Uint8 *g, Uint8 *b);

/* The framebuffer, for a blit. */
const Uint8 *host_framebuffer(void);

/* ------------------------------------------------ device callbacks
 *
 * The core calls these for any access to the device page. A front end does
 * not implement its own; it calls host_init once and feeds input through the
 * functions above.
 */

Uint8 host_dei(void *ctx, Uint8 addr);
void  host_deo(void *ctx, Uint8 addr, Uint8 val);

#endif /* HOST_H */