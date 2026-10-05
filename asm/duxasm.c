/* duxasm.c - a two-pass assembler for the Dux instruction set.
 *
 * Plain C89, no dependencies. Reads a source file, writes a flat ROM image
 * loaded at the reset vector.
 *
 * Two passes. The first assigns addresses to labels and records the byte
 * length of every line; the second resolves references and emits code.
 * Instruction sizes are always deterministic here, because operands are
 * lowered into fixed-width push sequences, so no backpatching is needed.
 *
 * Usage: duxasm [-l] [-o out.rom] in.tal
 */

#include "../dux.h"
#include "stackcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ shapes
 *
 * How many inline operands a mnemonic takes, and how they are lowered onto
 * the stack. The core pops addresses from the top, so a store's operands are
 * pushed in reverse of how they read.
 */

enum {
	S_NONE,   /* no inline operands                                    */
	S_LITB,   /* one byte literal, pushed                             */
	S_LITS,   /* one short literal, pushed                            */
	S_H,      /* push the high byte of a literal                     */
	S_A8,     /* one byte address: LDZ, LDR, DEI                      */
	S_A16,    /* one short address: LDA                               */
	S_M8,     /* byte address + mode-width value: STZ, STR, DEO      */
	S_M16,    /* short address + mode-width value: STA                */
	S_DEV,    /* device port: byte index, value width = mode        */
	S_ZP,     /* zero page: byte index; width follows the value   */
	S_JNZ,    /* jump to an inline target if the stack top is not 0  */
	S_JP,     /* jump target, lowered to LIT2 + JMP2/JSR2             */
	S_JMP,    /* inline relative jump: JMI, JCI, JSI                  */
	S_IMM     /* inline literal kept in the instruction stream       */
};

typedef struct {
	const char *name;
	Uint8 code;
	int  contextual;  /* base opcode is 0x00, the mode bits are the op */
	int  shape;
} OpDef;

static const OpDef ops[] = {
	{ "BRK", BRK, 1, S_NONE },

	{ "INC",  INC,  0, S_NONE }, { "POP",  POP,  0, S_NONE },
	{ "NIP",  NIP,  0, S_NONE }, { "SWP",  SWP,  0, S_NONE },
	{ "ROT",  ROT,  0, S_NONE }, { "DUP",  DUP,  0, S_NONE },
	{ "OVR",  OVR,  0, S_NONE },

	{ "EQU",  EQU,  0, S_NONE }, { "NEQ",  NEQ,  0, S_NONE },
	{ "GTH",  GTH,  0, S_NONE }, { "LTH",  LTH,  0, S_NONE },

	{ "JMP",  JMP,  0, S_JP   }, { "JCN",  JCN,  0, S_NONE },
	{ "JNZ",  JCN,  0, S_JNZ  },
	{ "JSR",  JSR,  0, S_JP   },
	{ "STH",  STH,  0, S_NONE },
	{ "JMP2r2", JMP | MODE_SHORT | MODE_RETURN, 0, S_NONE },

	{ "LDZ",  LDZ,  0, S_A8   }, { "STZ",  STZ,  0, S_ZP  },
	{ "LDR",  LDR,  0, S_A8   }, { "STR",  STR,  0, S_M8  },
	{ "LDA",  LDA,  0, S_A16  }, { "STA",  STA,  0, S_M16 },
	{ "DEI",  DEI,  0, S_A8   }, { "DEO",  DEO,  0, S_DEV  },

	{ "ADD",  ADD,  0, S_NONE }, { "SUB",  SUB,  0, S_NONE },
	{ "MUL",  MUL,  0, S_NONE }, { "DIV",  DIV,  0, S_NONE },
	{ "AND",  AND,  0, S_NONE }, { "ORA",  ORA,  0, S_NONE },
	{ "EOR",  EOR,  0, S_NONE }, { "SFT",  SFT,  0, S_NONE },

	{ "LIT",   OP_LIT,   1, S_LITB },
	{ "LIT2",  OP_LIT2,  1, S_LITS },
	{ "LITr",  OP_LITR,  1, S_LITB },
	{ "HI",   0,        0, S_H    },
	{ "LIT2r", OP_LIT2R, 1, S_LITS },
	{ "JMI",   OP_JMI,   1, S_JMP  },
	{ "JCI",   OP_JCI,   1, S_JMP  },
	{ "JSI",   OP_JSI,   1, S_JMP  }
};

#define NOPS ((int)(sizeof ops / sizeof ops[0]))

/* ---------------------------------------------------------------- symbols */

#define MAXSYM  1024
#define NAMELEN 64

typedef struct {
	char name[NAMELEN];
	long value;
	int  defined;
} Sym;

static Sym syms[MAXSYM];
static int nsyms;

/* The file currently being assembled, so a diagnostic can say where. */
static const char *cur_file = "";

/* The most recent "@name" label. A "&name" on a line of its own is filed
   under it as "parent.name", and a "&name" reference resolves to that same
   compound name. That is what keeps one routine's jumps from colliding with
   another's, without every label having to spell out the routine's name.
   Empty when no parent is in scope, in which case &name is just name. */
static char cur_parent[NAMELEN];

/* Join cur_parent and name into out, which must hold NAMELEN bytes. Returns 0
   if there is no scope, or if the result would not fit. */
static int scope_name(char *out, const char *name) {
	size_t n = strlen(cur_parent);
	size_t m = strlen(name);
	if(!n) return 0;
	if(n + 1 + m + 1 > NAMELEN) return 0;
	memcpy(out, cur_parent, n);
	out[n] = '.';
	memcpy(out + n + 1, name, m + 1);
	return 1;
}

static int errors;

/* --------------------------------------------------- zero page width lint
 *
 * The zero page is a byte array with no types, so a sixteen-bit counter and an
 * eight-bit counter live in it the same way and nothing complains when one line
 * reads a name as a short and another writes it as a byte. That mistake is
 * silent and nasty: the byte lands in the high half, the counter jumps to 256
 * on its first step, and any loop bounded by it either never runs or never
 * stops.
 *
 * So every symbolic zero-page operand is recorded with the width it was used at,
 * and a name seen at both widths is reported. The check is on names only: a
 * literal index has nothing to compare against, and a wrong literal is caught
 * the first time the program misbehaves.
 */
#define ZPMAX 64
static struct { char name[NAMELEN]; unsigned seen; } zplint[ZPMAX];
static int nzp;
static int zpwarnings;

static void zplint_note(const char *tok, int wide, int line) {
	char name[NAMELEN];
	size_t n;
	const char *t = tok;
	int i;

	if(*t == '&' || *t == '$') t++;
	if(*t == '&' || *t == '$' || !*t) return;
	if(isdigit((int)(Uint8)*t)) return;          /* a literal, not a name */
	n = strlen(t);
	if(n >= NAMELEN) return;
	while(n && (t[n-1] == ',' || t[n-1] == ']' || t[n-1] == ')')) n--;
	if(!n) return;
	memcpy(name, t, n);
	name[n] = 0;

	for(i = 0; i < nzp; i++)
		if(!strcmp(zplint[i].name, name)) break;
	if(i == nzp) {
		if(nzp >= ZPMAX) return;
		strcpy(zplint[nzp].name, name);
		zplint[nzp].seen = 0;
		nzp++;
	}

	/* Bit 0 is a byte use, bit 1 a short use, bit 4 remembers that the clash
	   has already been reported so one name does not print a line per line. */
	{
		unsigned bit = wide ? 2 : 1;
		if(zplint[i].seen & bit) return;             /* same width again */
		if(!(zplint[i].seen & 4) && (zplint[i].seen & 3) == (bit ^ 3)) {
			zplint[i].seen |= 4;
			if(zpwarnings++ < 20)
				fprintf(stderr,
				        "duxasm: %s:%d: warning: zero page '%s' is used as "
				        "a byte and as a short\n", cur_file, line, name);
		}
		zplint[i].seen |= bit;
	}
}

static void stack_report(void *ctx, const char *msg, int addr) {
	(void)ctx;
	fprintf(stderr, "duxasm: stack: %s\n", msg);
	(void)addr;
}

static void zplint_reset(void) {
	nzp = 0;
	zpwarnings = 0;
}

static void fail(int line, const char *fmt, const char *arg) {
	fprintf(stderr, "duxasm: %s:%d: ", cur_file, line);
	fprintf(stderr, fmt, arg);
	fprintf(stderr, "\n");
	errors++;
}

static Sym *sym_find(const char *name) {
	int i;
	for(i = 0; i < nsyms; i++)
		if(!strcmp(syms[i].name, name)) return &syms[i];
	return 0;
}

static Sym *sym_add(const char *name) {
	Sym *s = sym_find(name);
	if(s) return s;
	if(nsyms >= MAXSYM) {
		fprintf(stderr, "duxasm: too many symbols\n");
		exit(1);
	}
	s = &syms[nsyms++];
	strncpy(s->name, name, NAMELEN - 1);
	s->name[NAMELEN - 1] = 0;
	s->defined = 0;
	return s;
}

/* ------------------------------------------------------------------ lexer */

#define MAXLINE 512

typedef struct {
	char  text[MAXLINE];
	int   line;
} Line;

static int upper(int c) {
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* Hyphens are allowed in names, so "&on-frame" is one symbol. To keep that
   from colliding with arithmetic, '+' '-' '*' and '/' only count as operators
   when whitespace separates them from the term beside them: "$a - 1"
   subtracts, but "$a-1" is a single name. The shift pairs >> and << need no
   such care, since no name contains them. */
static int is_idchar(int c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '?' ||
	       c == '-';
}

/* Forward references are legal, so a symbol is only an error if it is still
   undefined after both passes. Pass 1 may see unresolved names. */
static int pass_no;

static int resolve(Sym *s, int line) {
	if(!s) {
		if(pass_no == 2) fail(line, "undefined symbol", "");
		return 1;
	}
	if(!s->defined) {
		if(pass_no == 2) fail(line, "symbol used before definition", s->name);
		return 1;
	}
	return 0;
}

/* --------------------------------------------------------------- emission */

static Uint8 rom[MEM_SIZE];
static long pc_now;

static void emit(int b) {
	if(pc_now >= MEM_SIZE) { errors++; return; }
	rom[pc_now++] = (Uint8)b;
}

static void emit_word(long v) {
	emit((int)((v >> 8) & 0xff));   /* memory is big-endian */
	emit((int)(v & 0xff));
}

/* Byte length of a push of the given width. */
static int push_len(int wide) { return wide ? 3 : 2; }

/* --------------------------------------------------------------- line work
 *
 * Each pass walks the same lines. `size_only` makes the first pass compute
 * lengths without caring whether a reference resolves yet.
 */

typedef struct {
	OpDef *op;
	Uint8  mode;    /* accumulated mode bits            */
	int    wide;    /* operands are shorts               */
	char   args[128];
} Insn;

/* Longest opcode name that prefixes tok, so "LIT" does not shadow "LIT2". */
static OpDef *find_op(const char *tok) {
	int i;
	size_t best = 0;
	OpDef *hit = 0;
	for(i = 0; i < NOPS; i++) {
		size_t l = strlen(ops[i].name);
		if(!strncmp(tok, ops[i].name, l) && l > best) {
			best = l;
			hit = (OpDef *)&ops[i];
		}
	}
	return hit;
}

/* Split a mnemonic into base name and mode suffix, longest name first. */
static void parse_mnemonic(const char *tok, Insn *in) {
	OpDef *o = find_op(tok);
	const char *p = tok;

	in->op = o;
	in->mode = 0;
	if(o) p = tok + strlen(o->name);

	while(*p) {
		int c = upper(*p++);
		if(c == '2') in->mode |= MODE_SHORT;
		else if(c == 'R') in->mode |= MODE_RETURN;
		else if(c == 'K') in->mode |= MODE_KEEP;
	}
}

/* Strip leading and trailing whitespace in place, shifting the text down. */
static void trim(char *s);

static void copytok(char *dst, const char *src, size_t cap);

/* Copy at most cap-1 bytes and NUL-terminate. */
static void copytok(char *dst, const char *src, size_t cap) {
	size_t i = 0;
	while(src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}

static void trim(char *s) {
	size_t n, lead = 0;
	while(s[lead] == ' ' || s[lead] == '\t') lead++;
	if(lead) memmove(s, s + lead, strlen(s + lead) + 1);
	n = strlen(s);
	while(n && (s[n - 1] == ' '  || s[n - 1] == '\t' ||
	            s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

/* Value of one hex digit. */
static int hexval(int c) {
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Read a numeric literal at t: $hex, %binary, or plain decimal. Returns the
   number of bytes consumed and stores the value, or returns 0 if no literal
   starts here.
 *
 * A literal is read as a prefix, not as a whole token, because in "$3 * 2" the
 * literal stops where the operator begins. The one ambiguity is a symbol whose
 * name starts with hex digits: "$banner" begins "$ba", but a letter or dot
 * straight after the digits means it was a name all along, so no literal is
   claimed. */
static size_t lit(const char *t, long *out) {
	size_t n = 0;
	long v = 0;

	if(t[0] == '$' || t[0] == '#') {
		n = 1;
		while(hexval((Uint8)t[n]) >= 0) {
			v = v * 16 + hexval((Uint8)t[n]);
			n++;
		}
		if(n == 1) return 0;                       /* no digits at all */
		if(isalpha((Uint8)t[n]) || t[n] == '_' || t[n] == '.')
			return 0;                              /* a name, not a number */
		*out = v;
		return n;
	}
	if(t[0] == '%' && (t[1] == '0' || t[1] == '1')) {
		n = 1;
		while(t[n] == '0' || t[n] == '1') {
			v = v * 2 + (t[n] - '0');
			n++;
		}
		*out = v;
		return n;
	}
	while(isdigit((Uint8)t[n])) {
		v = v * 10 + (t[n] - '0');
		n++;
	}
	if(!n) return 0;
	*out = v;
	return n;
}

/* ---------------------------------------------------------- expressions
 *
 * Operands are small expressions, not single terms. That is what makes
 * "&SCREEN.addr + 1" or "#&pattern >> 8" possible, which in turn is what
 * keeps the device ports pleasant to use.
 *
 *   expression := primary ( ("+" | "-" | "*" | "/" | ">>" | "<<") primary )*
 *
 * Precedence is deliberately flat: every operator has the same weight and
 * associates left to right. An assembler for a machine this small does not
 * need a grammar, and flat parsing is easier to reason about when every
 * operand is a single byte.
 */

/* One primary value: a literal, or a symbol reference behind a $, #, & or @
   sigil. Sets *ok to 0 if nothing could be read, and *used to the number of
   bytes consumed so the caller can step over exactly what was read.
 *
 * The function reports its own length because only it can tell a number from
   a name, and that decision needs the parse: "$2a" is the number 42, while
   "$banner" starts with the hex digits "ba" but is a symbol after all. */
static long primary(const char *tok, int line, size_t *used, int *ok) {
	Sym  *s;
	long  v;
	size_t n;
	char  name[NAMELEN];
	const char *start = tok;
	int neg = 0;

	*ok = 0;
	*used = 0;
	if(!tok || !*tok) return 0;

	/* A sign belongs to the primary, so "-1" is the value -1 rather than the
	   subtraction of 1 from nothing, and "- -1" nests cleanly. */
	while(*tok == '-' || *tok == '+') {
		if(*tok == '-') neg = !neg;
		tok++;
		while(*tok == ' ' || *tok == '\t') tok++;   /* "- &label" */
		if(!*tok) return 0;
	}

	/* A literal wins when one starts here. */
	n = lit(tok, &v);
	if(n) { *ok = 1; *used = (size_t)(tok + n - start); return neg ? -v : v; }

	/* Otherwise a symbol. All four sigils introduce one, so "$ZP.cursor" and
	   "#&pattern" are references rather than numbers. Only a leading '&' is
	   scoped: "&loop" means the loop in this routine, while "$loop" asks for
	   that name outright. */
	{
	int scoped = 0;
	while(*tok == '$' || *tok == '#' || *tok == '&' || *tok == '@') {
		if(*tok == '&') scoped = 1;
		if(!tok[1]) return 0;
		tok++;
	}

	/* A name begins with a letter or underscore, so "$1f" and "42" stay
	   numbers. This must be tried before the decimal fallback, since
	   isalpha() also accepts a-f. */
	if(isalpha((Uint8)tok[0]) || tok[0] == '_') {
		n = 0;
		while(tok[n] && n < NAMELEN - 1 && is_idchar((Uint8)tok[n])) n++;
		if(tok[n] == 0 || tok[n] == ' ' || tok[n] == '\t') {
			/* Copy exactly n bytes. primary() leaves the expression in place
			   rather than terminating it, so anything that copies up to the
			   next space would swallow the operator along with the name. */
			memcpy(name, tok, n);
			name[n] = 0;
			*used = (size_t)(tok + n - start);
			*ok = 1;
			s = sym_find(name);
			/* Prefer the enclosing scope, but only when it really carries
			   that name; otherwise fall back to the plain one, so a bare
			   &name with no scope still resolves. */
			if(scoped) {
				char full[NAMELEN];
				if(scope_name(full, name)) {
					Sym *t = sym_find(full);
					if(t) s = t;
				}
			}
			/* An unresolved name is not a parse failure: pass 1 is still
			   filling in later definitions. resolve() reports it only once
			   pass 2 has had its chance. */
			v = resolve(s, line) ? 0 : s->value;
			return neg ? -v : v;
		}
	}
	}
	return 0;
}

/* If an operator sits at *pp, consume it and return a code for it:
   '+' '-' '*' '/' as themselves, '>>' as 'r', '<<' as 'l', else 0. */
static int take_op(char **pp) {
	char *p = *pp;
	int op;
	if(p[0] == '>' && p[1] == '>') { *pp = p + 2; return 'r'; }
	if(p[0] == '<' && p[1] == '<') { *pp = p + 2; return 'l'; }
	if(p[0] == '+' || p[0] == '-' || p[0] == '*' || p[0] == '/') {
		op = *p;
		*pp = p + 1;
		return op;
	}
	return 0;
}

/* Evaluate a whole operand expression, stepping over each primary by the
   length primary() reports.
 *
 * Pass 1 is a measuring pass, so an unresolved symbol is taken as zero and
   measured through rather than abandoned. Stopping instead would make pass 1
   disagree with pass 2 about where the line ends, and every label after it
 * would look like a duplicate. resolve() is what reports, and only in
   pass 2. */
static long operand(const char *tok, int line) {
	char  work[MAXLINE];
	char *p;
	long  acc;
	size_t used;
	int   ok;

	if(!tok || !*tok) return 0;
	copytok(work, tok, sizeof work);

	/* One primary, then any number of operator-primary pairs. The first
	   primary sits outside the loop because it is not optional: what follows
	   it must be an operator or the end of the expression, never another
	   primary. */
	p = work;
	while(*p == ' ' || *p == '\t') p++;

	acc = primary(p, line, &used, &ok);
	if(!ok) { fail(line, "cannot parse operand", p); return 0; }
	p += used;

	for(;;) {
		long rhs;
		int  op;
		int  spaced = 0;

		while(*p == ' ' || *p == '\t') { p++; spaced = 1; }
		if(!*p) break;

		/* Only '-' insists on surrounding whitespace, because it is the one
		   operator character a name may contain: "$a - 1" subtracts, but
		   "$a-1" is a single name. No name can contain '*', '/' or '+', so
		   those need no spacing to read as arithmetic. */
		op = take_op(&p);
		if(!op || (!spaced && op == '-')) {
			fail(line, "expected an operator", p);
			return 0;
		}
		while(*p == ' ' || *p == '\t') p++;

		rhs = primary(p, line, &used, &ok);
		if(!ok) { fail(line, "operator needs an operand", p); return 0; }
		p += used;

		if(op == 'r' || op == 'l') {
			if(rhs < 0 || rhs > 31) {
				fail(line, "shift count out of range", p);
				return 0;
			}
			if(op == 'r') acc >>= rhs; else acc <<= rhs;
			continue;
		}
		switch(op) {
		case '+': acc += rhs; break;
		case '-': acc -= rhs; break;
		case '*': acc *= rhs; break;
		case '/':
			if(rhs == 0) { fail(line, "divide by zero", p); return 0; }
			acc /= rhs;
			break;
		}
	}
	return acc;
}

/* Is tok a standalone operator rather than a value? */
static int is_op_token(const char *t) {
	if(!t || !*t) return 0;
	if(!strcmp(t, "+") || !strcmp(t, "-") || !strcmp(t, "*") || !strcmp(t, "/"))
		return 1;
	if(!strcmp(t, ">>") || !strcmp(t, "<<")) return 1;
	return 0;
}

/* ------------------------------------------------------- operand splitting
 *
 * Operands are separated by whitespace or commas, but whitespace *around* an
 * operator belongs to the expression, so "$a + 1" is one operand and
 * "STA $10 $20" is two. Splitting on separators first and only then looking
 * for operators is what keeps those two cases apart; scanning characters and
 * trying to recognise operators in passing conflates them.
 */

#define MAXTOK 64

/* Split s in place on whitespace and commas. Returns the token count; each
   token is NUL-terminated and points into s. */
static int split_toks(char *s, char *tok[MAXTOK], int line) {
	char *p = s;
	int n = 0;
	if(!s) return 0;
	for(;;) {
		while(*p == ' ' || *p == '\t' || *p == ',') p++;
		if(!*p) break;
		/* Too many items is an error, not a short list. This used to stop at
		   MAXTOK and return what it had, so a DB line of seventeen values
		   assembled to sixteen with no message at all: asm/bounce.tal's
		   seventeen-column wordmark lost its last column that way, and the
		   ROM rendered a "D" with no right-hand edge - thirty-six pixels
		   short, and nothing anywhere said why. A silent truncation is worse
		   than a refusal, because the symptom shows up as a picture and not
		   as an error. */
		if(n >= MAXTOK) {
			fail(line, "more than 64 items on one line; put the rest on the next", "");
			return n;
		}
		tok[n++] = p;
		while(*p && *p != ' ' && *p != '\t' && *p != ',') p++;
		if(*p) *p++ = 0;
	}
	return n;
}

/* Append a token to an expression buffer, with bounds checking. */
static void app(char *dst, size_t *len, size_t cap, const char *s) {
	while(*s && *len + 1 < cap) dst[(*len)++] = *s++;
	dst[*len] = 0;
}

/* Split an operand list into up to two expressions, each a NUL-terminated
   string of the form "primary", "primary op primary", and so on. Returns the
   number of operands found, 0, 1 or 2. */
static int split_args(char *s, char *a[2], int line) {
	static char buf[2][MAXLINE];
	char *tok[MAXTOK];
	int ntok, i = 0, slot = 0;

	a[0] = 0;
	a[1] = 0;
	if(!s) return 0;
	ntok = split_toks(s, tok, line);

	while(slot < 2 && i < ntok) {
		size_t len = 0;
		buf[slot][0] = 0;
		/* A sign standing alone, as in "- &label", opens the expression
		   rather than starting an operand of its own. */
		if(is_op_token(tok[i]) && i + 1 < ntok) {
			app(buf[slot], &len, sizeof buf[slot], tok[i]);
			app(buf[slot], &len, sizeof buf[slot], " ");
			i++;
			app(buf[slot], &len, sizeof buf[slot], tok[i]);
			i++;
		} else {
			app(buf[slot], &len, sizeof buf[slot], tok[i]);
			i++;
		}
		/* Absorb any operator-primary pairs. A trailing operator with
		   nothing after it is left for operand() to report. */
		while(i + 1 < ntok && is_op_token(tok[i])) {
			app(buf[slot], &len, sizeof buf[slot], " ");
			app(buf[slot], &len, sizeof buf[slot], tok[i]);
			app(buf[slot], &len, sizeof buf[slot], " ");
			app(buf[slot], &len, sizeof buf[slot], tok[i + 1]);
			i += 2;
		}
		if(i < ntok && is_op_token(tok[i])) i++;  /* let operand() complain */
		a[slot] = buf[slot];
		slot++;
	}
	return slot;
}

/* Emit a data directive. Returns bytes emitted. */
static int data_directive(const char *mn, char *args, int line, int size_only) {
	char *a[2];
	char work[MAXLINE];
	int count;

	strncpy(work, args, sizeof work - 1);
	work[sizeof work - 1] = 0;
	trim(work);

	if(!strcmp(mn, "TEXT") || !strcmp(mn, "STR") || !strcmp(mn, "STRZ")) {
		/* Unescape into a separate buffer. Shrinking in place would leave a
		   stale tail, and the emit loop would pick it up. */
		char *p = work;
		char out[MAXLINE];
		int n = 0;
		if(*p != '"') { fail(line, "TEXT needs a quoted string", ""); return 0; }
		p++;
		while(*p && *p != '"') {
			int c = *p++;
			if(c == '\\' && *p) {
				c = *p++;
				if(c == 'n') c = '\n';
				else if(c == 't') c = '\t';
				else if(c == '0') c = 0;
				else if(c == 'r') c = '\r';
				else if(c == '\\' || c == '"') c = c;
			}
			if(n < MAXLINE - 1) out[n++] = (char)c;
		}
		for(count = 0; count < n; count++) emit((Uint8)out[count]);
		/* STRZ terminates, which is what Console/print expects. Two strings
		   written back to back with TEXT and no NUL between them run into
		   each other, and the symptom is a message that repeats the ones
		   after it. */
		if(!strcmp(mn, "STRZ")) { emit(0); return n + 1; }
		return n;
	}

	if(!strcmp(mn, "DB") || !strcmp(mn, "BYTE")) {
		/* Each comma-separated field is a full expression, so a table can
		   hold computed values. split_toks does the separating; operand()
		   does the evaluating. */
		char *tok[MAXTOK];
		int ntok = split_toks(work, tok, line);
		int i;
		for(i = 0; i < ntok; i++) {
			emit((int)(operand(tok[i], line) & 0xff));
		}
		return ntok;
	}

	if(!strcmp(mn, "DW") || !strcmp(mn, "WORD")) {
		/* Every comma-separated field is emitted, exactly as DB does for
		   bytes. Taking only the first field is the same as a DB that
		   quietly dropped everything after the comma: the table comes out
		   the right shape and full of zeros, and nothing complains. */
		char *tok[MAXTOK];
		int ntok = split_toks(work, tok, line);
		int i;
		if(!size_only)
			for(i = 0; i < ntok; i++)
				emit_word(operand(tok[i], line));
		return ntok * 2;
	}

	count = split_args(work, a, line);
	if(!strcmp(mn, "FILL")) {
		long n = operand(a[0], line);
		long v = a[1][0] ? operand(a[1], line) : 0;
		long i;
		/* In pass 1 only report the length; b may reference a forward
		   symbol that is not resolvable yet. */
		if(!size_only)
			for(i = 0; i < n; i++) emit((int)(v & 0xff));
		return (int)n;
	}
	(void)size_only;
	(void)count;
	fail(line, "unknown directive", mn);
	return 0;
}

/* --------------------------------------------------------------- assemble
 *
 * Walk one line. In pass 1 we only accumulate `size`; in pass 2 we emit.
 */

static long assemble_line(char *text, int lineno, long here, int size_only) {
	char work[MAXLINE];
	char *p, *tok;
	char name[NAMELEN];
	Insn in;
	OpDef *o;
	int nargs;
	char *a[2];
	long size = 0;

	strncpy(work, text, sizeof work - 1);
	work[sizeof work - 1] = 0;

	/* strip comments: everything from the first semicolon */
	for(p = work; *p; p++) {
		if(*p == ';') { *p = 0; break; }
	}
	trim(work);
	if(!work[0]) return 0;

	/* Symbol definitions. "@name" labels the current address; "@name = v"
	   defines a constant. A bare "&name" on its own line also labels the
	   current address, which is how sublabels read best. In operand position
	   "&name" is a reference instead. */
	while(work[0] == '@' || work[0] == '.' || work[0] == '&') {
		size_t n = 0;
		int sigil = work[0];
		p = work + 1;
		while(*p && n < NAMELEN - 1 && is_idchar((Uint8)*p)) name[n++] = *p++;
		name[n] = 0;
		if(!n) { fail(lineno, "empty label name", ""); return 0; }
		p = work + 1 + n;
		/* A trailing "&name" is a sublabel only when nothing follows it.
		   "&name = v" is a constant; "ADD &name" is a reference. */
		if(sigil == '&') {
			char *q = p;
			while(*q == ' ' || *q == '\t') q++;
			if(*q && *q != '=') break;      /* an operand, not a label */
		}
		while(*p == ' ' || *p == '\t') p++;
		if(*p == '=') {                     /* constant */
			char full[NAMELEN];
			const char *label = name;
			Sym *s;
			if(sigil != '@' && scope_name(full, name)) label = full;
			s = sym_add(label);
			p++;
			while(*p == ' ' || *p == '\t') p++;
			{
				char rhs[MAXLINE];
				int k = 0;
				while(*p && k < MAXLINE - 1) rhs[k++] = *p++;
				rhs[k] = 0;
				s->value = operand(rhs, lineno);
				s->defined = 1;
			}
			/* A constant emits nothing, so the location counter does not
			   move. Continue scanning the rest of the line. */
			memmove(work, p, strlen(p) + 1);
			trim(work);
			if(!work[0]) return 0;
			continue;
		}
		{
			char full[NAMELEN];
			const char *label = name;
			Sym *s;
			/* "@name" opens a scope; "&name" and ".name" nest inside it. */
			if(sigil != '@' && scope_name(full, name)) label = full;
			s = sym_add(label);
			if(s->defined && s->value != here)
				fail(lineno, "duplicate label", label);
			s->value = here;
			s->defined = 1;
			/* Only "@name" opens a scope. A "&name" nests inside the scope
			   that is already open but does not become one itself, so a
			   routine with a dozen &branch labels keeps all of them under the
			   routine rather than under whichever branch came last. */
			if(sigil == '@') copytok(cur_parent, label, NAMELEN);
		}
		/* keep scanning for further definitions on this line */
		memmove(work, p, strlen(p) + 1);
		trim(work);
		if(!work[0]) return 0;
	}

	p = work;
	tok = p;
	while(*p && *p != ' ' && *p != '\t') p++;
	if(*p) { *p = 0; p++; }

	/* strip a leading '&' or bare label reference used as a statement */
	if(tok[0] == '&') {
		tok++;
	}

	/* Directives. Match case-insensitively so "TEXT" and "text" both work. */
	{
		static const struct { const char *name; const char *kind; } dirs[] = {
			{ "DB",   "DB"   }, { "BYTE", "DB"   },
			{ "DW",   "DW"   }, { "WORD", "DW"   },
			{ "TEXT", "TEXT" }, { "STR",  "TEXT" },
			{ "STRZ", "STRZ" }, { "TEXTZ", "STRZ" },
			{ "FILL", "FILL" }
		};
		int di;
		for(di = 0; di < (int)(sizeof dirs / sizeof dirs[0]); di++) {
			size_t l = strlen(dirs[di].name);
			if(!strncmp(tok, dirs[di].name, l) && !is_idchar((Uint8)tok[l])) {
				return data_directive(dirs[di].kind, p, lineno, size_only);
			}
		}
	}

	/* location counter: |addr */
	if(tok[0] == '|') {
		long v = operand(tok + 1, lineno);
		if(!size_only) pc_now = v;
		return v - here;
	}

	/* mnemonic */
	memset(&in, 0, sizeof in);
	parse_mnemonic(tok, &in);
	o = in.op;
	if(!o) {
		fail(lineno, "unknown mnemonic", tok);
		return 0;
	}

	strncpy(in.args, p, sizeof in.args - 1);
	in.args[sizeof in.args - 1] = 0;
	trim(in.args);
	nargs = split_args(in.args, a, lineno);

	/* Width: short if the "2" suffix asked for it, or if the value being
	   moved cannot be a byte.
	   Only the value decides. An address above 255 says nothing about the
	   width of a result: "LDA &label" is nearly always a 16-bit address, and
	   widening it because of that would leave two bytes on the stack where
	   the programmer meant one. So shapes that take an address are driven by
	   the mode alone. */
	in.wide = (in.mode & MODE_SHORT) ? 1 : 0;
	if(!in.wide) {
		long v = 0;
		int  wide_by_value = 0;

		if(o->shape == S_LITB) {
			v = operand(a[0], lineno);
			wide_by_value = (v > 255 || v < -128);
		} else if((o->shape == S_M8 || o->shape == S_M16 ||
		           o->shape == S_ZP) && nargs > 1) {
			v = operand(a[1], lineno);      /* the value, not the address */
			wide_by_value = (v > 255 || v < -128);
		}
		if(wide_by_value) in.wide = 1;
	}
	if(in.wide) in.mode |= MODE_SHORT;

	/* Only pass 2: pass 1 would report every name twice. LDZ shares its shape
	   with LDR, which takes a PC offset rather than a zero page address, so the
	   mnemonic is what distinguishes the two. */
	if(pass_no == 2 && nargs >= 1 &&
	   (!strcmp(o->name, "LDZ") || !strcmp(o->name, "STZ")))
		zplint_note(a[0], in.wide, lineno);

	switch(o->shape) {
	case S_NONE:
		size = 1;
		if(!size_only) emit((int)(o->code | in.mode));
		break;

	case S_LITB:
		if(nargs != 1) { fail(lineno, "needs one operand", o->name); return 0; }
		size = push_len(0);
		if(!size_only) {
			emit(OP_LIT);
			emit((int)(operand(a[0], lineno) & 0xff));
		}
		break;

	case S_H:
		/* The high byte of a literal, for splitting a short across two
		   device ports. Emits a plain LIT with that byte inline. */
		if(nargs != 1) { fail(lineno, "needs one operand", "HI"); return 0; }
		size = push_len(0);
		if(!size_only) {
			emit(OP_LIT);
			emit((int)((operand(a[0], lineno) >> 8) & 0xff));
		}
		break;

	case S_LITS:
		if(nargs != 1) { fail(lineno, "needs one operand", o->name); return 0; }
		size = push_len(1);
		if(!size_only) {
			emit(OP_LIT2);
			emit_word(operand(a[0], lineno));
		}
		break;

	case S_A8:
		/* With no operand the port comes off the stack, which is how a
		   program reaches a port whose number is only known at run time.
		   DEI is always 8-bit: the core pops a single byte for the index. */
		if(nargs == 0) {
			size = 1;
			if(!size_only) emit((int)(o->code | in.mode));
			break;
		}
		if(nargs != 1) { fail(lineno, "needs one operand", o->name); return 0; }
		size = push_len(0) + 1;
		if(!size_only) {
			emit(OP_LIT);
			emit((int)(operand(a[0], lineno) & 0xff));
			emit((int)(o->code | in.mode));
		}
		break;

	case S_A16:
		/* LDA always addresses with a short; the 2 suffix selects the width
		   of the value that comes back. The two are independent, which is
		   why "LDA2 &x" reads two bytes and "LDA &x" reads one.
		   With no operand the address comes off the stack instead, which is
		   the only way to reach a computed address: the core's LDA pops a
		   16-bit address in every mode. Keeping a suffix gives LDAk, which
		   leaves the address on the stack, so a loop over a table is a push,
		   a load and an increment. */
		if(nargs == 0) {
			size = 1;
			if(!size_only) emit((int)(o->code | in.mode));
			break;
		}
		if(nargs != 1) { fail(lineno, "needs one operand", o->name); return 0; }
		size = push_len(1) + 1;
		if(!size_only) {
			emit(OP_LIT2);
			emit_word(operand(a[0], lineno));
			emit((int)(o->code | in.mode));
		}
		break;

	case S_DEV:
		/* The port index is always 8-bit. Push the value first, then the
		   port, because the core pops the address from the top. A "2"
		   suffix widens the value but never the port: each half of a port
		   pair is its own port, so a program that needs both bytes writes
		   them in turn. */
		if(nargs == 0) {
			/* Both the port and the value come off the stack. This is the
			   only way to write a device short from a computed value: STA
			   addresses flat memory, so "STA $2c" would hit zero page and
			   never reach the device page at 0x012c. */
			size = 1;
			if(!size_only) emit((int)(o->code | in.mode));
			break;
		}
		if(nargs == 1) {
			/* Port only; the value is already on the stack at whatever width
			   the mode calls for. The port itself is always 8-bit, so the
			   "2" suffix changes nothing here. Size is one opcode plus a
			   LIT and its byte. */
			size = 2 + 1;
			if(!size_only) {
				emit(OP_LIT); emit((int)(operand(a[0], lineno) & 0xff));
				emit((int)(o->code | in.mode));
			}
			break;
		}
		if(nargs != 2) { fail(lineno, "needs a port", o->name); return 0; }
		if(in.wide) {
			size = 3 + 2 + 1;
			if(!size_only) {
				emit(OP_LIT2); emit_word(operand(a[1], lineno));  /* value */
				emit(OP_LIT);  emit((int)(operand(a[0], lineno) & 0xff)); /* port */
				emit((int)(o->code | in.mode));
			}
		} else {
			size = 2 + 2 + 1;
			if(!size_only) {
				emit(OP_LIT); emit((int)(operand(a[1], lineno) & 0xff));  /* value */
				emit(OP_LIT); emit((int)(operand(a[0], lineno) & 0xff));  /* port  */
				emit((int)(o->code | in.mode));
			}
		}
		break;

	case S_ZP:
		if(nargs == 1) {
			size = push_len(0) + 1;
			if(!size_only) {
				emit(OP_LIT); emit((int)(operand(a[0], lineno) & 0xff));
				emit((int)(o->code | in.mode));
			}
			break;
		}
		if(nargs != 2) { fail(lineno, "needs addr and value", o->name); return 0; }
		if(in.wide) {
			size = 3 + 3 + 1;
			if(!size_only) {
				emit(OP_LIT2); emit_word(operand(a[1], lineno));
				emit(OP_LIT2); emit_word(operand(a[0], lineno));
				emit((int)(o->code | MODE_SHORT));
			}
		} else {
			size = 2 + 2 + 1;
			if(!size_only) {
				emit(OP_LIT); emit((int)(operand(a[1], lineno) & 0xff));
				emit(OP_LIT); emit((int)(operand(a[0], lineno) & 0xff));
				emit((int)(o->code | in.mode));
			}
		}
		break;

	case S_M8:
	case S_M16:
		/* The address width belongs to the shape: STR takes a byte offset
		   from PC, STA a full 16-bit address. The value width follows the
		   mode. With no operand both the value and the address come off the
		   stack, address first. */
		if(nargs == 0) {
			size = 1;
			if(!size_only) emit((int)(o->code | in.mode));
			break;
		}
		if(nargs == 1) {
			/* One operand: the address only, the value is already on the
			   stack. */
			size = (o->shape == S_M16 ? 3 : 2) + 1;
			if(!size_only) {
				if(o->shape == S_M16) {
					emit(OP_LIT2); emit_word(operand(a[0], lineno));
				} else {
					emit(OP_LIT); emit((int)(operand(a[0], lineno) & 0xff));
				}
				emit((int)(o->code | in.mode));
			}
			break;
		}
		if(nargs != 2) { fail(lineno, "needs addr and value", o->name); return 0; }
		size = (o->shape == S_M16 ? 3 : 2) + push_len(in.wide) + 1;
		if(!size_only) {
			if(in.wide) { emit(OP_LIT2); emit_word(operand(a[1], lineno)); }
			else        { emit(OP_LIT);  emit((int)(operand(a[1], lineno) & 0xff)); }
			if(o->shape == S_M16) {
				emit(OP_LIT2); emit_word(operand(a[0], lineno));
			} else {
				emit(OP_LIT); emit((int)(operand(a[0], lineno) & 0xff));
			}
			emit((int)(o->code | in.mode));
		}
		break;

	case S_JNZ:
		/* The core's JCN pops the target and then the condition. In byte
		   mode the target is a single signed byte treated as an offset from
		   PC, so an absolute target needs the short form, which pops two
		   bytes.

		   Lowering the target here is also the only way a source line can
		   express the order: the target has to sit *under* the condition,
		   and the condition is already on the stack by the time the
		   assembler sees the operands. */
		if(nargs != 1) { fail(lineno, "needs one target", o->name); return 0; }
		size = 3 + 1;
		if(!size_only) {
			emit(OP_LIT2);
			emit_word(operand(a[0], lineno));
			emit((int)(JCN | MODE_SHORT));
		}
		break;

	case S_JP:
		if(nargs != 1) { fail(lineno, "needs one target", o->name); return 0; }
		size = 3 + 1;
		if(!size_only) {
			emit(OP_LIT2);
			emit_word(operand(a[0], lineno));
			emit((int)(o->code | MODE_SHORT));
		}
		break;

	case S_JMP:
		if(nargs != 1) { fail(lineno, "needs one target", o->name); return 0; }
		size = 3;
		if(!size_only) {
			long target = operand(a[0], lineno);
			long next = here + 3;
			emit((int)o->code);
			emit_word((long)(Sint16)(target - next));  /* relative to next */
		}
		break;

	case S_IMM:
	default:
		size = 1;
		if(!size_only) emit((int)(o->code | in.mode));
		break;
	}
	return size;
}

/* ------------------------------------------------------------------- driver */

/* --------------------------------------------------------------- includes
 *
 * INCLUDE "path" splices another source file in at that point. Sprite data
 * lives in its own file because it is generated art rather than code, and
 * copying it inline would mean regenerating the game every time the pony
 * changes shape.
 *
 * Line numbering restarts inside each included file, so a diagnostic names both
 * the file and the line within it and both are the ones a person would check.
 */

#define MAXINC 8

/* If text is an INCLUDE directive, copy its path into out and return 1. */
static int try_include(const char *text, char *out, int cap) {
	const char *p = text;
	int n = 0;

	while(*p == ' ' || *p == '\t') p++;
	if(strncmp(p, "INCLUDE", 7) != 0) return 0;
	p += 7;
	while(*p == ' ' || *p == '\t') p++;
	if(*p != '"') return 0;
	p++;
	while(*p && *p != '"') {
		if(n >= cap - 1) return 0;
		out[n++] = *p++;
	}
	out[n] = 0;
	return 1;
}

static int do_pass(FILE *f, int size_only, const char *top_name) {
	Line ln;
	long here = (long)RESET_VECTOR;
	FILE *stack[MAXINC];
	const char *names[MAXINC];
	int lines[MAXINC];
	char parents[MAXINC][NAMELEN];
	int depth = 0;
	int lineno = 0;

	cur_file = top_name;

	/* pc_now is the emitter's own cursor: assemble_line advances it as it
	   emits. In pass 1 nothing is emitted, so here tracks the address and
	   the two must not be conflated. */
	pc_now = (long)RESET_VECTOR;
	pass_no = size_only ? 1 : 2;

	for(;;) {
		char path[MAXLINE];

		if(!fgets(ln.text, sizeof ln.text, f)) {
			if(depth == 0) break;
			fclose(f);
			f = stack[--depth];
			cur_file = names[depth];
			lineno = lines[depth];
			/* Restore the enclosing scope, so a "&label" in the including
			   file still means what it meant before. */
			copytok(cur_parent, parents[depth], NAMELEN);
			continue;
		}
		ln.line = ++lineno;

		if(try_include(ln.text, path, sizeof path)) {
			FILE *nf;
			if(depth >= MAXINC) {
				fail(ln.line, "includes nested too deeply", path);
				return errors;
			}
			nf = fopen(path, "r");
			if(!nf) {
				/* Not found as given, so try it beside the file that asked.
				   A program that says INCLUDE "common.tal" means the copy next
				   to it, not whatever happens to be in the working directory:
				   otherwise the same source assembles one way from the top of
				   the tree and another from anywhere else, which is the worst
				   kind of bug to chase. */
				char dir[MAXLINE];
				const char *slash = strrchr(cur_file, '/');
				if(slash && (size_t)(slash - cur_file) < sizeof dir - strlen(path) - 2) {
					memcpy(dir, cur_file, (size_t)(slash - cur_file));
					dir[slash - cur_file] = 0;
					strcat(dir, "/");
					strcat(dir, path);
					nf = fopen(dir, "r");
					if(nf) {
						strncpy(path, dir, sizeof path - 1);
						path[sizeof path - 1] = 0;
					}
				}
			}
			if(!nf) {
				fail(ln.line, "cannot open", path);
				return errors;
			}
			names[depth] = cur_file;
			lines[depth] = lineno;
			copytok(parents[depth], cur_parent, NAMELEN);
			stack[depth++] = f;
			f = nf;
			cur_file = path;
			lineno = 0;
			/* An included file starts with no enclosing scope. Without this a
			   table of "&name" labels at the end of a program would file
			   itself under whichever routine happened to be defined last. */
			cur_parent[0] = 0;
			continue;
		}

		if(size_only) here += assemble_line(ln.text, ln.line, here, 1);
		else          assemble_line(ln.text, ln.line, pc_now, 0);
	}

	while(depth > 0) fclose(stack[--depth]);
	return errors;
}

int main(int argc, char **argv) {
	const char *in = 0, *out = "out.rom";
	int listing = 0, symbols = 0, check_stack = 1;
	int i;
	FILE *f;

	for(i = 1; i < argc; i++) {
		if(!strcmp(argv[i], "-l")) { listing = 1; continue; }
		if(!strcmp(argv[i], "-S")) { symbols = 1; continue; }
		if(!strcmp(argv[i], "-X")) { check_stack = 1; continue; }
		if(!strcmp(argv[i], "+X")) { check_stack = 1; continue; }
		if(!strcmp(argv[i], "-o") && i + 1 < argc) { out = argv[++i]; continue; }
		if(argv[i][0] != '-') { in = argv[i]; continue; }
		fprintf(stderr, "duxasm: unknown option %s\n", argv[i]);
		return 1;
	}
	if(!in) {
		fprintf(stderr, "usage: duxasm [-l] [-S] [-o out.rom] in.tal\n");
		return 1;
	}

	f = fopen(in, "r");
	if(!f) { fprintf(stderr, "duxasm: cannot open %s\n", in); return 1; }

	cur_file = in;
	zplint_reset();
	do_pass(f, 1, in);
	fclose(f);

	if(errors) {
		fprintf(stderr, "duxasm: %d error(s), no output written\n", errors);
		return 1;
	}

	memset(rom, 0, sizeof rom);
	f = fopen(in, "r");
	if(!f) { fprintf(stderr, "duxasm: cannot reopen %s\n", in); return 1; }
	cur_file = in;
	do_pass(f, 0, in);
	fclose(f);

	if(errors) {
		fprintf(stderr, "duxasm: %d error(s) in pass 2, no output written\n", errors);
		return 1;
	}

	if(zpwarnings)
		fprintf(stderr, "duxasm: %d zero page width warning(s)\n", zpwarnings);

	if(check_stack) {
		int bad = stack_check(rom, RESET_VECTOR, (Uint16)pc_now,
		                      0, 0, stack_report, 0);
		if(bad) {
			fprintf(stderr, "duxasm: %d stack problem(s)\n", bad);
			errors++;
		}
		if(stack_check_warnings())
			fprintf(stderr, "duxasm: %d stack warning(s): leftovers at a "
			        "break, on a path that may never run\n",
			        stack_check_warnings());
	}

	f = fopen(out, "wb");
	if(!f) { fprintf(stderr, "duxasm: cannot write %s\n", out); return 1; }
	/* write only what the program occupies */
	fwrite(rom + RAM_BASE, 1, (size_t)(pc_now - RAM_BASE), f);
	fclose(f);

	if(listing) {
		fprintf(stderr, "duxasm: %s -> %s (%ld bytes at %04x), %d symbols\n",
		        in, out, pc_now - RAM_BASE, (int)pc_now, nsyms);
	}
	if(symbols) {
		int k;
		fprintf(stderr, "\nsymbols:\n");
		for(k = 0; k < nsyms; k++)
			fprintf(stderr, "  %04x  %-32s %s\n", (int)syms[k].value,
		        syms[k].name,
		        syms[k].defined ? "" : "(undefined)");
	}
	return 0;
}