/* stackcheck.c - does every path through a program leave the stack where it
 * found it?
 *
 * A stack machine has nowhere to put a variable, so the depth of the stack is
 * the only bookkeeping a program has, and every mistake in it is silent. A push
 * with no matching pop leaves a byte behind that nothing ever looks at; a pop
 * with no matching push takes one more than was pushed, and because both
 * pointers are bytes, that simply wraps. Either way the program runs, draws
 * most of what it meant to, and misbehaves somewhere unrelated an hour later.
 *
 * The host notices leftovers at the end of a vector, but only on the paths it
 * happens to run, and only once the program is already wrong. So this walks the
 * code instead, from the reset vector and from every device vector, and reports
 * the mistakes that are always mistakes:
 *
 *   - an address reached with two different depths. Some path pushed or popped
 *     something the others did not, so from there on the two disagree about
 *     what is on the stack. This is the one worth having: it catches the bad
 *     line even when neither disagreeing path is ever executed. A loop that
 *     grows its stack shows up here too, at its own head.
 *   - a pop from a stack that is not that deep, and a push past the end.
 *   - a break with anything left on either stack. A vector is entered with both
 *     stacks empty and has to be left that way.
 *
 * Two things make it more than a stack height counter.
 *
 * Branch targets are not read from the instruction word; they are popped off
 * the stack. "JMP2 label" assembles to "LIT2 label" followed by a jump that pops
 * it. So the walk carries a shadow of the working stack holding the value of
 * every byte whose value it knows. A literal is known; the result of an
 * addition is not tracked, because the moment a target is computed rather than
 * written down the walk stops following and says nothing, which is the honest
 * answer for a checker this blind.
 *
 * And a call's effect on the working stack has to be worked out, not guessed.
 * "LIT2 value / LIT port / JSR2 &put16" leaves two bytes on the stack that the
 * routine then consumes, so the code after the call is not at the depth it was
 * before the call, and a checker that assumes otherwise is wrong about
 * everything downstream. So the callee is walked first and its return depth is
 * used as the caller's new depth. That is what makes the depth comparison
 * meaningful across a call at all.
 *
 * Device vectors are entry points too, and nothing in the program branches to
 * one: the host calls it. A write to a vector port therefore starts a walk
 * there as well. Doing that here rather than in the assembler means it works for
 * a vector installed through a helper just as well as for one written out by
 * hand, since by the time the write reaches the core the port and the value are
 * two numbers on a stack like any other.
 *
 * It is a checker, not a verifier. It counts bytes and has no types, so it
 * cannot tell a push of the wrong width from a push of the right one; that
 * blindness is shared with the CPU on purpose. Data is not walked into: a jump
 * into the middle of a sprite would report nonsense, and a program is entitled
 * to jump into a table. Reachability is what decides what is code.
 */

#include "stackcheck.h"
#include "../dux.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define UNKNOWN  (-1)

/* Leftovers at a break, which may be on a path no frame takes. See the BRK
   case in walk(). Counted here rather than returned, so the one return value
   stays "how many things are definitely wrong". */
static int warnings;
#define MAXSTEP  4000000
#define MAXWORK  (MEM_SIZE - RAM_BASE)
#define MAXCALL  32              /* nesting of inferred calls */

/* What the walk knows about each address it has reached. */
typedef struct {
	short w;                 /* working stack depth, or UNKNOWN */
	short w2;                /* a second depth, if one turned up */
	short ret;               /* depth this routine returns at, or UNKNOWN */
	unsigned char bad;       /* already reported something here */
	unsigned char isret;     /* a walk of this address reached a return */
} Fact;

/* A queued address together with the depths it was reached with. Reading those
   back out of the fact table does not work: a queue entry for a branch target
   is added before anything has walked it, so the table has nothing to say. */
typedef struct {
	Uint16 pc;
	short w;
	short r;
} QEnt;

/* One byte of the working stack: its value, if the walk can pin it down. */
typedef struct {
	unsigned short v;
	unsigned char known;
} Byte;

/* How an opcode's pushed bytes relate to what it popped. Only enough to keep
   the shadow honest; anything not listed is treated as unknown. */
enum { V_UNKNOWN, V_LITERAL };

/* The device vector ports, whose base address is the low half of the pair. */
static const Uint8 vector_ports[] = { 0x20, 0x30, 0x80, 0x90, 0xa0, 0xc0 };

/* Fill in the byte counts an opcode moves on each stack. Returns the
   instruction length, which is 1 for every opcode except the immediate family:
   the whole design puts 16-bit operands on the stack rather than in the
   instruction word. Zero means "this byte is not an instruction start". */
static int effect(Uint8 op, int *wp, int *wpu, int *rp, int *rpu, int *literal) {
	Uint8 base = (Uint8)(op & OPCODE_MASK);
	int two = (op & MODE_SHORT) ? 1 : 0;
	int ret = (op & MODE_RETURN) ? 1 : 0;
	int w = two ? 2 : 1;         /* width of a popped or pushed value */
	int onret = ret ? 1 : 0;     /* which stack this form works on */

	*wp = *wpu = *rp = *rpu = 0;
	*literal = 0;

	/* Opcode zero is contextual: the same five bits are a break, one of the
	   immediates, or one of the three inline jump forms, decided by the mode
	   bits. Those forms have their own cases in the core rather than going
	   through the mode macros, so for them the mode bits belong to the form
	   and not to the operands. Keep mode does not arise: the assembler never
	   emits it, and it would alias an immediate with a jump. */
	if(base == BRK) {
		switch(op & 0xe0) {
		case 0x00:  return 1;                        /* BRK            */
		case 0x20:  *wp = 1; return 3;                /* OP_JCI: a cond */
		case 0x40:  return 3;                         /* OP_JMI         */
		case 0x60:  *rpu = 2; return 3;               /* OP_JSI: a ret  */
		case 0x80:  *wpu = 1; *literal = 1; return 2; /* OP_LIT         */
		case 0xa0:  *wpu = 2; *literal = 1; return 3; /* OP_LIT2        */
		case 0xc0:  *rpu = 1; return 2;               /* OP_LITR        */
		case 0xe0:  *rpu = 2; return 3;               /* OP_LIT2R       */
		}
		return 0;
	}

	switch(base) {
	case INC:
		if(onret) { *rp = w; *rpu = w; } else { *wp = w; *wpu = w; }
		break;
	case POP:
		if(onret) *rp = w; else *wp = w;
		break;
	case NIP:
		if(onret) *rp = 2 * w; else *wp = 2 * w;
		break;
	case SWP: case ROT:
		if(onret) { *rp = 2 * w; *rpu = 2 * w; }
		else      { *wp = 2 * w; *wpu = 2 * w; }
		break;
	case DUP:
		/* Duplicates the top item: one in, two out. */
		if(onret) { *rp = w; *rpu = 2 * w; } else { *wp = w; *wpu = 2 * w; }
		break;
	case OVR:
		if(onret) { *rp = 2 * w; *rpu = 2 * w; } else { *wp = 2 * w; *wpu = 2 * w; }
		break;

	case EQU: case NEQ: case GTH: case LTH:
		if(onret) { *rp = 2 * w; *rpu = 1; } else { *wp = 2 * w; *wpu = 1; }
		break;

	case JMP:
		if(onret) *rp = w; else *wp = w;
		break;
	case JCN:
		if(onret) *rp = w + 1; else *wp = w + 1;
		break;
	case JSR:
		/* Pops the target from one stack, pushes the return address on the
		   other, high byte first. */
		if(onret) { *rp = w; *wpu = 2; } else { *wp = w; *rpu = 2; }
		break;
	case STH:
		/* Copies from one stack to the other. */
		if(onret) { *wp = w; *rpu = w; } else { *rp = w; *wpu = w; }
		break;

	case LDZ: case LDR: case DEI:
		/* An eight-bit index or port, then a value of the mode's width. */
		if(onret) { *rp = 1; *rpu = w; } else { *wp = 1; *wpu = w; }
		break;
	case LDA:
		/* A 16-bit address, then a value of the mode's width. */
		if(onret) { *rp = 2; *rpu = w; } else { *wp = 2; *wpu = w; }
		break;
	case STZ: case STR:
		/* The address and the value both come off the stack. */
		if(onret) { *rp = 1; *wp = w; } else { *wp = 1 + w; }
		break;
	case STA:
		if(onret) { *rp = 2; *wp = w; } else { *wp = 2 + w; }
		break;
	case DEO:
		/* The port is one byte whatever the mode; only the value widens. */
		if(onret) *rp = 1 + w; else *wp = 1 + w;
		break;

	case ADD: case SUB: case MUL: case DIV:
	case AND: case ORA: case EOR:
		if(onret) { *rp = 2 * w; *rpu = w; } else { *wp = 2 * w; *wpu = w; }
		break;
	case SFT:
		/* One narrow control byte, then a value of the mode's width. */
		if(onret) { *rp = 1 + w; *rpu = w; } else { *wp = 1 + w; *wpu = w; }
		break;

	default:
		return 0;
	}
	return 1;
}

/* Is this port the base of a device vector? */
static int is_vector(Uint8 port) {
	int i;
	for(i = 0; i < (int)(sizeof vector_ports / sizeof vector_ports[0]); i++)
		if(vector_ports[i] == port) return 1;
	return 0;
}

/* Everything the walk shares. Held in one place so the recursive call below can
   reach it without threading six parameters through. */
typedef struct {
	const Uint8 *rom;
	Uint16 limit;
	Fact *fact;
	/* Branches to follow, shared with the recursive call below so that a
	   branch inside a walked routine is still followed. */
	QEnt *todo;
	int nwork;
	int depth;                /* how deep the inferred calls have gone */
	long budget;
	int problems;
	int warnings;
	void (*report)(void *, const char *, int);
	void *ctx;
} Walk;

/* Walk one path from pc with the given depths.
 *
 * "stop" says what ends the path early:
 *   0  follow branches, queue them, keep going -- the top level
 *   1  stop at the first return and report the working depth there, which is
 *      how a call's effect on the caller's stack is worked out
 *
 * Returns the working depth at the return, or UNKNOWN if the path ended some
 * other way, so the caller cannot know what depth to carry on at. */
/* Note that this address is being walked at this working depth. Returns 1 if
 * that pair was already noted, so a caller can skip the work.
 *
 * Two facts are kept per address, which is all that is needed and all that is
 * safe. One is the obvious half: a loop that jumps backwards would otherwise be
 * walked over and over until the step budget ran out, and the check would end in
 * silence - which reads exactly like a clean pass. @clear, the first thing
 * every program calls, hid a whole screen of code from the checker that way.
 *
 * The second is that being reached at two different depths is not itself
 * wrong. A routine called from two places may be handed two different numbers
 * of bytes, and cube hands its @widen one byte from nine call sites and three
 * from a tenth - all ten correct. What says a call site is wrong is the pop
 * inside the routine that runs off the bottom, so the routine is walked at each
 * depth it is reached at and reports for itself. After two depths an address is
 * left alone, so neither a loop nor a leak can make the walk unbounded.
 */
static int depth_note(Walk *wk, Uint16 pc, int wd) {
	Fact *f;
	if(pc < RAM_BASE || pc >= wk->limit) return 1;
	f = &wk->fact[pc - RAM_BASE];
	if(f->w == UNKNOWN)  { f->w = (short)wd;  return 0; }
	if(f->w == (short)wd) return 1;
	if(f->w2 == UNKNOWN) { f->w2 = (short)wd; return 0; }
	if(f->w2 == (short)wd) return 1;
	return 1;
}

static int walk(Walk *wk, Uint16 pc, int wd, int rd, int stop) {
	/* The shadow belongs to this path and not to the whole walk. Walking a
	   callee recursively would otherwise zero the caller's shadow on the way
	   in, and every branch target after the call would read as unknown - so
	   the walk would stop following and say nothing, which looks exactly like
	   a clean pass. That is what @clear, the first thing every program calls,
	   used to do to the whole check. */
	Byte sh[STACK_SIZE];
	Uint16 entry = pc;
	int i;

	for(i = 0; i < STACK_SIZE; i++) { sh[i].v = 0; sh[i].known = 0; }

	for(;;) {
		Uint8 op;
		int wp, wpu, rp, rpu, lit, len;
		Uint16 target = 0, fall;
		char buf[128];
		Fact *f;

		if(pc < RAM_BASE || pc >= wk->limit) break;
		if(getenv("DUX_CHECK_TRACE"))
			fprintf(stderr, "walk %04x w=%d r=%d op=%02x\n", pc, wd, rd, wk->rom[pc]);
		if(--wk->budget <= 0) break;
		wk->budget--;

		/* The depth at this address, before the instruction runs. Reaching an
		   address at a second depth is ordinary - see depth_note - so the
		   only thing to do here is record it and walk on. */
		f = &wk->fact[pc - RAM_BASE];
		(void)depth_note(wk, pc, wd);

		op = wk->rom[pc];
		len = effect(op, &wp, &wpu, &rp, &rpu, &lit);
		if(len == 0) break;                    /* data, not an opcode */
		fall = (Uint16)(pc + len);

		if(wp > wd) {
			if(!f->bad) {
				sprintf(buf, "%04x pops %d byte%s from a stack %d deep",
				        pc, wp, wp == 1 ? "" : "s", wd);
				wk->report(wk->ctx, buf, pc);
				f->bad = 1;
				wk->problems++;
			}
			break;
		}
		if(rp > rd) {
			if(!f->bad) {
				sprintf(buf, "%04x pops %d byte%s from a return stack %d deep",
				        pc, rp, rp == 1 ? "" : "s", rd);
				wk->report(wk->ctx, buf, pc);
				f->bad = 1;
				wk->problems++;
			}
			break;
		}

		/* A short write to a vector port installs an entry point: the host
		   will call it, and nothing in the program branches there. */
		if((op & OPCODE_MASK) == DEO && (op & MODE_SHORT) && wd >= 3 &&
		   sh[wd - 1].known && is_vector((Uint8)sh[wd - 1].v) &&
		   sh[wd - 2].known && sh[wd - 3].known) {
			Uint16 v = (Uint16)((sh[wd - 3].v << 8) | sh[wd - 2].v);
			if(v >= RAM_BASE && v < wk->limit &&
			   wk->fact[v - RAM_BASE].w == UNKNOWN && wk->nwork < MAXWORK) {
				wk->fact[v - RAM_BASE].w = 0;
				wk->todo[wk->nwork].pc = v; wk->todo[wk->nwork].w = 0;
				wk->todo[wk->nwork].r = 0; wk->nwork++;
			}
		}

		/* A branch target is whatever is on top of the shadow right now, so
		   it has to be read before the pops overwrite it. */
		if((op & OPCODE_MASK) == JMP || (op & OPCODE_MASK) == JCN ||
		   (op & OPCODE_MASK) == JSR) {
			int w = (op & MODE_SHORT) ? 2 : 1;
			int have = 0;
			if(!(op & MODE_RETURN)) {
				if(w == 2 && wd >= 2 && sh[wd - 2].known && sh[wd - 1].known) {
					target = (Uint16)((sh[wd - 2].v << 8) | sh[wd - 1].v);
					have = 1;
				} else if(w == 1 && wd >= 1 && sh[wd - 1].known) {
					target = sh[wd - 1].v;
					have = 1;
				}
			}
			if(!have) target = 0;
		}

		wd -= wp;
		rd -= rp;

		/* Push results. A literal is worth knowing, because a branch target
		   is a literal followed by a jump that pops it; anything else is
		   arithmetic the walk does not model. */
		for(i = 0; i < wpu; i++) {
			sh[wd + i].known = (unsigned char)(lit != 0);
			sh[wd + i].v = 0;
		}
		if(lit) {
			if(wpu == 1) sh[wd].v = wk->rom[pc + 1];
			if(wpu == 2) {
				sh[wd].v = wk->rom[pc + 1];   /* high byte first */
				sh[wd + 1].v = wk->rom[pc + 2];
			}
		}
		wd += wpu;
		rd += rpu;

		if(wd > STACK_SIZE - 1 || rd > STACK_SIZE - 1) {
			if(!f->bad) {
				sprintf(buf, "%04x pushes past the end of the stack (%d, %d)",
				        pc, wd, rd);
				wk->report(wk->ctx, buf, pc);
				f->bad = 1;
				wk->problems++;
			}
			break;
		}

		/* ---- where control goes next ---- */

		if(op == BRK) {
			if(wd != 0 || rd != 0) {
				if(!f->bad) {
					sprintf(buf, "break at %04x with %d byte%s on the stack "
					        "and %d on the return stack",
					        pc, wd, wd == 1 ? "" : "s", rd);
					/* A warning, not a problem. Popping off the bottom of
					   the stack reads whatever is below it and is always
					   wrong; leaving something on the stack at a break is
					   only wrong on the paths that are actually taken, and
					   this walk follows both arms of every branch - so a
					   loop that runs six times on the machine and twice on
					   paper reports leftovers that no frame ever has. cube
					   does. The host's own balance check, which runs the
					   real thing, is what settles it. */
					wk->report(wk->ctx, buf, pc);
					f->bad = 1;
					wk->warnings++;
				}
			}
			break;
		}

		if((op & OPCODE_MASK) == JMP && (op & MODE_RETURN)) {
			/* A return. The target comes off the return stack, which the
			   walk does not track by value, so the path ends here; where it
			   was called from is followed by the caller of this walk.

			   Popping the address deeper than the return stack goes was
			   caught above, so there is nothing else to say here. */
			if(entry >= RAM_BASE && entry < wk->limit) {
				wk->fact[entry - RAM_BASE].isret = 1;
				wk->fact[entry - RAM_BASE].ret = (short)wd;
			}
			return stop ? wd : UNKNOWN;
		}

		if(op == OP_LIT || op == OP_LIT2) { pc = fall; continue; }

		if((op & OPCODE_MASK) == BRK) {          /* inline jump forms */
			Uint16 off = wk->rom[pc + 1] | ((Uint16)wk->rom[pc + 2] << 8);
			if((op & 0xe0) == 0x20) {            /* OP_JCI: both ways */
				Uint16 a = (Uint16)(pc + 3 + (Sint16)off);
				if(!depth_note(wk, a, wd) && wk->nwork < MAXWORK) {
					wk->todo[wk->nwork].pc = a;
					wk->todo[wk->nwork].w = (short)wd;
					wk->todo[wk->nwork].r = (short)rd;
					wk->nwork++;
				}
				pc = (Uint16)(pc + 3);
			} else {                               /* OP_JMI, OP_JSI */
				pc = (Uint16)(pc + 3 + (Sint16)off);
			}
			if(pc >= RAM_BASE && pc < wk->limit) continue;
			break;
		}

		if((op & OPCODE_MASK) == JMP) {
			/* A target the walk could pin down, or the end of the path when
			   it could not. */
			if(target >= RAM_BASE && target < wk->limit) {
				/* An unconditional jump is walked as a call and its depth
				   carried straight out, because a jump does not come back:
				   whatever it reaches hands its return to our caller. That
				   is what makes "JMP2 @plot" at the end of @plotat work -
				   the walk follows @plot to its return and hands @plotat's
				   caller the depth @plot returns at. */
				if(!depth_note(wk, target, wd) && wk->depth < MAXCALL) {
					int r;
					wk->depth++;
					r = walk(wk, target, wd, rd, 1);
					wk->depth--;
					if(stop && r != UNKNOWN) return r;
					break;
				}
				if(depth_note(wk, target, wd)) {
					/* Ground already covered at this depth. If the target is
					   a routine - something a walk reached a return in -
					   then this is a tail call and its return depth is ours
					   too. Otherwise it is a loop's back edge, and carrying
					   on down this path is what walks the rest of the loop
					   body and the code after it. */
					Fact *t = &wk->fact[target - RAM_BASE];
					if(t->isret) {
						if(stop && t->ret != UNKNOWN) return t->ret;
						break;
					}
					if(target >= entry && target <= pc) {
						pc = fall; continue;
					}
					break;
				}
				pc = target; continue;
			}
			break;
		}

		if((op & OPCODE_MASK) == JCN) {
			if(!depth_note(wk, target, wd) && wk->nwork < MAXWORK) {
				wk->todo[wk->nwork].pc = target;
				wk->todo[wk->nwork].w = (short)wd;
				wk->todo[wk->nwork].r = (short)rd;
				wk->nwork++;
			}
			pc = fall;
			continue;
		}

		if((op & OPCODE_MASK) == JSR) {
			/* Work out the call's effect on the working stack by walking the
			   routine, then carry on here at whatever depth it returns with.
			   A routine that cannot be walked to a return leaves the caller
			   with nothing to carry, and the path ends. */
			int ret = UNKNOWN;
			/* The return address belongs to the routine, which is about to
			   pop it, so the caller carries on without it. The routine is
			   walked with it there, because that is what makes the check for
			   popping it too deep mean anything. */
			if(target >= RAM_BASE && target < wk->limit && wk->depth < MAXCALL) {
				wk->depth++;
				ret = walk(wk, target, wd, rd, 1);
				wk->depth--;
			}
			rd -= rpu;
			if(ret == UNKNOWN) break;
			wd = ret;
			pc = fall;
			continue;
		}

		pc = fall;
	}
	return UNKNOWN;
}

int stack_check_warnings(void) { return warnings; }

int stack_check(const Uint8 *image, Uint16 start, Uint16 limit,
                const Uint16 *roots, int nroots,
                void (*report)(void *, const char *, int), void *ctx) {
	static Fact fact[MAXWORK];
	static QEnt work[MAXWORK];
	/* The walker's own branch queue, kept separate from work: both are indexed
	   from zero, so sharing one array lets a branch queued inside a walk
	   overwrite a vector the outer loop has not reached yet. */
	static QEnt todo[MAXWORK];
	Walk wk;
	int nwork = 0, i;

	/* MEM_SIZE is 65536, which does not fit in a Uint16, so the upper bound
	   cannot be written as a comparison against it: a limit is a Uint16 and is
	   therefore always inside the address space already. */
	if(start < RAM_BASE) return 0;
	if(limit <= start) return 0;

	memset(fact, 0, sizeof fact);
	for(i = 0; i < MAXWORK; i++) {
		fact[i].w = UNKNOWN; fact[i].w2 = UNKNOWN; fact[i].ret = UNKNOWN;
	}

	memset(&wk, 0, sizeof wk);
	wk.rom = image;
	wk.limit = limit;
	wk.fact = fact;
	wk.report = report;
	wk.ctx = ctx;
	wk.budget = MAXSTEP;

	/* A queue of branches to follow, shared with the recursive call above so
	   that a branch inside a walked routine is still followed. */
	wk.todo = todo;
	wk.nwork = 0;
	wk.depth = 0;

	fact[start - RAM_BASE].w = 0;
	work[nwork].pc = start; work[nwork].w = 0; work[nwork].r = 0; nwork++;

	/* A vector is entered the way the reset vector is: with nothing on either
	   stack, because the host calls it that way. */
	for(i = 0; i < nroots; i++) {
		Uint16 r = roots[i];
		if(r < RAM_BASE || r >= limit) continue;
		if(fact[r - RAM_BASE].w == UNKNOWN) {
			fact[r - RAM_BASE].w = 0;
			work[nwork].pc = r; work[nwork].w = 0; work[nwork].r = 0; nwork++;
		}
	}

	while(nwork) {
		Uint16 pc = work[--nwork].pc;
		int wd = work[nwork].w;
		int rd = work[nwork].r;

		if(pc < RAM_BASE || pc >= limit) continue;
		walk(&wk, pc, wd, rd, 0);

		/* Drain whatever the walk queued, including from inside routines. */
		while(wk.nwork) {
			/* Entries are pushed at the index the counter points at and then
			   the counter is advanced, so the last one is one below it. */
			Uint16 q = wk.todo[--wk.nwork].pc;
			short qw = wk.todo[wk.nwork].w, qr = wk.todo[wk.nwork].r;
			if(q < RAM_BASE || q >= limit) continue;
			walk(&wk, q, qw, qr, 0);
		}
	}

	warnings = wk.warnings;
	return wk.problems;
}