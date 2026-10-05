/* stackcheck.h - static stack balance check for assembled programs */

#ifndef DUX_STACKCHECK_H
#define DUX_STACKCHECK_H

#include "../dux.h"

/* Walk the code from start up to limit, following control flow, and report
   every place where two paths disagree about the stack depth, every return with
   something on the return stack, and every break with something left on the
   working stack.

   "roots" lists further entry points, which a device vector is: the host runs
   one every frame without ever branching to it, so nothing in the program
   connects it to the reset vector and a walk from the reset vector alone would
   never reach it. The count may be zero and the list may be null.

   "report" is called with a one line message, the address, and the context
   pointer. Returns the number of problems found. */
/* Leftovers at a break: reported, but not counted as problems. */
int stack_check_warnings(void);

int stack_check(const Uint8 *image, Uint16 start, Uint16 limit,
                const Uint16 *roots, int nroots,
                void (*report)(void *, const char *, int), void *ctx);

#endif