/* tests.c - conformance tests for the CPU core
 *
 * Each test loads a tiny program at the reset vector, runs it to BRK, then
 * inspects machine state. The core itself has no dependencies; only the test
 * harness talks to libc.
 *
 * Build: cc -std=c89 -Wall -Wextra -o tests tests.c dux.c
 */

#include "dux.h"
#include <stdio.h>

static int checks   = 0;
static int failures = 0;

/* Load a program at the reset vector and run it to BRK. */
static void run(const Uint8 *prog, int n) {
	int i;
	dux_reset();
	for(i = 0; i < n; i++)
		dux.mem[RESET_VECTOR + i] = prog[i];
	dux_eval((Uint16)RESET_VECTOR, 0);
}

/* Compare the working stack against an expected image, top element first.
   top_first[0] is the value a POP would return.

   Shorts live on the stack with their LOW byte on top, so the image for the
   value 0x0102 is { 0x02, 0x01 }. */
/* Compare the top n bytes, top first, and the depth as well. Every case here
   runs from an empty stack, so the depth is n exactly. Checking the bytes
   alone is not enough: an opcode that copied an item onto itself would leave
   the bytes it was given still sitting there and pass. */
static void expect_wst(const char *name, const Uint8 *top_first, int n) {
	int i, ok = 1;
	if(dux.wstp != n) ok = 0;
	for(i = 0; ok && i < n; i++) {
		if(dux.wst[(Uint8)(dux.wstp - 1 - i)] != top_first[i]) { ok = 0; break; }
	}
	checks++;
	if(!ok) {
		failures++;
		printf("FAIL %s\n", name);
		printf("  expected (top first):");
		for(i = 0; i < n; i++) printf(" %02x", top_first[i]);
		printf("\n  actual   (top first):");
		for(i = 0; i < n; i++)
			printf(" %02x", dux.wst[(Uint8)(dux.wstp - 1 - i)]);
		printf("\n");
	}
}

/* Assert an entire short by value, hiding the on-stack byte order. */
static void expect_short(const char *name, Uint16 want) {
	Uint8 img[2];
	img[0] = (Uint8)(want & 0xff);   /* low byte is on top */
	img[1] = (Uint8)(want >> 8);
	expect_wst(name, img, 2);
}

static void expect_u8(const char *name, Uint8 got, Uint8 want) {
	checks++;
	if(got != want) {
		failures++;
		printf("FAIL %s: got %02x, want %02x\n", name, got, want);
	}
}

static void expect_u16(const char *name, Uint16 got, Uint16 want) {
	checks++;
	if(got != want) {
		failures++;
		printf("FAIL %s: got %04x, want %04x\n", name, got, want);
	}
}

/* Opcode shorthands, so the test programs below read like assembly. */
#define LIT   0x80
#define LIT2  0xa0
#define STHr  0x4f
#define ADD   0x18
#define ADD2  0x38
#define ADDr  0x58
#define ADDk  0x98
#define SUB   0x19
#define SUB2  0x39
#define STZ2  0x31
#define LDZ2  0x30
#define MUL   0x1a
#define DIV   0x1b
#define AND   0x1c
#define ORA   0x1d
#define EOR   0x1e
#define SFT   0x1f
#define EQU   0x08
#define NEQ   0x09
#define GTH   0x0a
#define LTH   0x0b
#define EQU2  0x28
#define NEQ2  0x29
#define GTH2  0x2a
#define LTH2  0x2b
#define MUL2  0x3a
#define DIV2  0x3b
#define SFT2  0x3f
#define INC   0x01
#define POP   0x02
#define NIP   0x03
#define SWP   0x04
#define ROT   0x05
#define DUP   0x06
#define OVR   0x07
#define STH   0x0f
#define LDZ   0x10
#define STZ   0x11
#define LDA2  0x34
#define STA2  0x35
#define JSR   0x0e
#define JSR2  0x2e
#define JMP2  0x2c
#define JMP2r 0x4c
#define JMP2r2 0x6c
#define BRK   0x00
#define END   { BRK }

int main(void) {
	Uint8 s1[1], s2[2], s3[3];

	/* ---------------------------------------------------- arithmetic */
	{ Uint8 p[] = { LIT, 5, LIT, 3, ADD, BRK };
	  s1[0] = 8;  run(p, sizeof p);  expect_wst("add", s1, 1); }

	/* SUB computes top minus second, so 5 - 3. */
	{ Uint8 p[] = { LIT, 5, LIT, 3, SUB, BRK };
	  s1[0] = 2;  run(p, sizeof p);  expect_wst("sub", s1, 1); }

	{ Uint8 p[] = { LIT, 6, LIT, 7, MUL, BRK };
	  s1[0] = 42; run(p, sizeof p);  expect_wst("mul", s1, 1); }

	{ Uint8 p[] = { LIT, 9, LIT, 2, DIV, BRK };
	  s1[0] = 4;  run(p, sizeof p);  expect_wst("div", s1, 1); }

	/* Division by zero pushes zero rather than trapping. */
	{ Uint8 p[] = { LIT, 0, LIT, 5, DIV, BRK };
	  s1[0] = 0;  run(p, sizeof p);  expect_wst("div by zero", s1, 1); }

	{ Uint8 p[] = { LIT, 0xf0, LIT, 0x3c, AND, BRK };
	  s1[0] = 0x30; run(p, sizeof p); expect_wst("and", s1, 1); }

	{ Uint8 p[] = { LIT, 0xf0, LIT, 0x0f, ORA, BRK };
	  s1[0] = 0xff; run(p, sizeof p); expect_wst("ora", s1, 1); }

	{ Uint8 p[] = { LIT, 0xff, LIT, 0x0f, EOR, BRK };
	  s1[0] = 0xf0; run(p, sizeof p); expect_wst("eor", s1, 1); }

	/* SFT pops the control byte first (from the top), then the value.
	   Control high nibble shifts left, low nibble shifts right. */
	{ Uint8 p[] = { LIT, 0x01, LIT, 0x10, SFT, BRK };  /* shift 1 left by 1 */
	  s1[0] = 0x02; run(p, sizeof p); expect_wst("sft left", s1, 1); }

	{ Uint8 p[] = { LIT, 0x02, LIT, 0x01, SFT, BRK };  /* shift 2 right by 1 */
	  s1[0] = 0x01; run(p, sizeof p); expect_wst("sft right", s1, 1); }

	/* Right shift happens before left shift. Value 0x11, control 0x40:
	   0x11 >> 0 then << 4 = 0x10. */
	{ Uint8 p[] = { LIT, 0x11, LIT, 0x40, SFT, BRK };
	  s1[0] = 0x10; run(p, sizeof p); expect_wst("sft right then left", s1, 1); }

	/* Value 0x11, control 0x41: shift right 1, then left 4. */
	{ Uint8 p[] = { LIT, 0x11, LIT, 0x41, SFT, BRK };
	  s1[0] = 0x80; run(p, sizeof p); expect_wst("sft right1 left4", s1, 1); }

	/* Shifting an 8-bit value left by 4 overflows out of the byte. */
	{ Uint8 p[] = { LIT, 0x02, LIT, 0x40, SFT, BRK };
	  s1[0] = 0x20; run(p, sizeof p); expect_wst("sft left4", s1, 1); }

	/* -------------------------------------------------- short (2 byte) */
	/* LIT2 takes the value high byte first, but the low byte ends up on top
	   of the stack. 0x0001 + 0x0002 = 0x0003. */
	{ Uint8 p[] = { LIT2, 0x00, 0x01, LIT2, 0x00, 0x02, ADD2, BRK };
	  run(p, sizeof p);  expect_short("add2 16-bit", 0x0003); }

	/* Big-endian on input: "LIT2 0x34 0x12" is the value 0x3412. */
	{ Uint8 p[] = { LIT2, 0x34, 0x12, LIT2, 0x00, 0x00, ADD2, BRK };
	  run(p, sizeof p);  expect_short("add2 big value", 0x3412); }

	/* 0xffff + 1 wraps to zero, matching an 8-bit machine. */
	{ Uint8 p[] = { LIT2, 0xff, 0xff, LIT2, 0x00, 0x01, ADD2, BRK };
	  run(p, sizeof p);  expect_short("add2 wraps", 0x0000); }

	/* 16-bit subtraction across the byte boundary. */
	{ Uint8 p[] = { LIT2, 0x00, 0x01, LIT2, 0x00, 0x01, SUB2, BRK };
	  run(p, sizeof p);  expect_short("sub2 borrows", 0x0000); }

	/* ----------------------------------------------------- comparisons
	   Binary ops take the TOP of the stack as the FIRST operand. In the
	   notation "a b", b is on top, so GTH computes b > a. That means
	   "LIT 5, LIT 3, GTH" asks "is 3 greater than 5" -> 0. */
	{ Uint8 p[] = { LIT, 5, LIT, 3, GTH, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("gth true", s1, 1); }

	{ Uint8 p[] = { LIT, 3, LIT, 5, GTH, BRK };
	  s1[0] = 0;  run(p, sizeof p);  expect_wst("gth false", s1, 1); }

	{ Uint8 p[] = { LIT, 5, LIT, 5, EQU, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("equ", s1, 1); }

	{ Uint8 p[] = { LIT, 5, LIT, 5, NEQ, BRK };
	  s1[0] = 0;  run(p, sizeof p);  expect_wst("neq", s1, 1); }

	{ Uint8 p[] = { LIT, 3, LIT, 5, LTH, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("lth true", s1, 1); }

	{ Uint8 p[] = { LIT, 5, LIT, 3, LTH, BRK };
	  s1[0] = 0;  run(p, sizeof p);  expect_wst("lth false", s1, 1); }

	/* The short-mode comparisons. There were none of these, and the gap was
	   not theoretical: a whole self-check program compared shorts with a
	   byte-mode EQU, which silently compared only the low bytes and left a
	   byte on the stack each time. Both halves of the pair have to be right
	   for the result to mean anything, so these check the whole 16 bits and
	   not just the byte that happened to differ. */

	{ Uint8 p[] = { LIT2, 0x0f, 0x4f, LIT2, 0x0f, 0x4f, EQU2, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("equ2 same short", s1, 1); }

	/* Equal in the low byte, different in the high one. Byte mode would
	   call this equal; it is not. */
	{ Uint8 p[] = { LIT2, 0x0f, 0x4f, LIT2, 0x01, 0x4f, EQU2, BRK };
	  s1[0] = 0;  run(p, sizeof p);  expect_wst("equ2 high half differs", s1, 1); }

	{ Uint8 p[] = { LIT2, 0x0f, 0x4f, LIT2, 0x01, 0x4f, NEQ2, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("neq2 high half differs", s1, 1); }

	{ Uint8 p[] = { LIT2, 0x00, 0x05, LIT2, 0x00, 0x03, GTH2, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("gth2 true", s1, 1); }

	{ Uint8 p[] = { LIT2, 0x00, 0x03, LIT2, 0x00, 0x05, GTH2, BRK };
	  s1[0] = 0;  run(p, sizeof p);  expect_wst("gth2 false", s1, 1); }

	/* Comparisons are unsigned, so $ffff is the largest value there is and
	   not minus one. A test written as if shorts were signed sends every
	   negative value down the wrong branch. */
	{ Uint8 p[] = { LIT2, 0xff, 0xff, LIT2, 0x00, 0x00, GTH2, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("gth2 unsigned", s1, 1); }

	{ Uint8 p[] = { LIT2, 0x00, 0x00, LIT2, 0xff, 0xff, LTH2, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("lth2 unsigned", s1, 1); }

	/* One step of the high byte outranks $ff of the low one, which is the
	   case a byte-wise compare gets backwards. */
	{ Uint8 p[] = { LIT2, 0x01, 0x00, LIT2, 0x00, 0xff, GTH2, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("gth2 high byte first", s1, 1); }

	{ Uint8 p[] = { LIT2, 0x00, 0xff, LIT2, 0x01, 0x00, LTH2, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("lth2 high byte first", s1, 1); }

	/* MUL and DIV are b * a and b / a, with a the top of the stack, so the
	   first operand written is the left-hand one. */
	{ Uint8 p[] = { LIT2, 0x01, 0x40, LIT2, 0x00, 0x03, MUL2, BRK };
	  run(p, sizeof p);  expect_short("mul2", 0x03c0); }

	{ Uint8 p[] = { LIT2, 0x01, 0x00, LIT2, 0x00, 0x04, DIV2, BRK };
	  run(p, sizeof p);  expect_short("div2", 0x0040); }

	/* Dividing by zero pushes zero rather than trapping. */
	{ Uint8 p[] = { LIT2, 0x12, 0x34, LIT2, 0x00, 0x00, DIV2, BRK };
	  run(p, sizeof p);  expect_short("div2 by zero", 0x0000); }

	/* SFT2 takes one control byte: the high nibble shifts left, the low
	   nibble right. So $04 is a divide by four, not a shift right by four. */
	{ Uint8 p[] = { LIT2, 0x12, 0x34, LIT, 0x04, SFT2, BRK };
	  run(p, sizeof p);  expect_short("sft2 right 4", 0x0123); }

	{ Uint8 p[] = { LIT2, 0x00, 0x81, LIT, 0x10, SFT2, BRK };
	  run(p, sizeof p);  expect_short("sft2 left 1", 0x0102); }

	/* $01 is a shift *right* by one, not left: the low nibble counts right
	   and the high nibble counts left. Reading it the other way round is how
	   a divide by four turns into a divide by sixteen. */
	{ Uint8 p[] = { LIT2, 0x00, 0x81, LIT, 0x01, SFT2, BRK };
	  run(p, sizeof p);  expect_short("sft2 control $01 is right", 0x0040); }

	/* --------------------------------------------------------- shuffling */
	{ Uint8 p[] = { LIT, 1, LIT, 2, LIT, 3, ROT, BRK };
	  s3[0] = 1; s3[1] = 3; s3[2] = 2;
	  run(p, sizeof p);  expect_wst("rot", s3, 3); }

	/* OVR copies the second item to the top: a b -- a b a */
	{ Uint8 p[] = { LIT, 1, LIT, 2, OVR, BRK };
	  s3[0] = 1; s3[1] = 2; s3[2] = 1;
	  run(p, sizeof p);  expect_wst("ovr", s3, 3); }

	{ Uint8 p[] = { LIT, 1, LIT, 2, SWP, BRK };
	  s2[0] = 1; s2[1] = 2;
	  run(p, sizeof p);  expect_wst("swp", s2, 2); }

	{ Uint8 p[] = { LIT, 1, LIT, 2, NIP, BRK };
	  s1[0] = 2;  run(p, sizeof p);  expect_wst("nip keeps top", s1, 1); }

	{ Uint8 p[] = { LIT, 7, DUP, BRK };
	  s2[0] = 7; s2[1] = 7;
	  run(p, sizeof p);  expect_wst("dup", s2, 2); }

	/* POP discards the top item and leaves what was under it. */
	{ Uint8 p[] = { LIT, 1, LIT, 7, POP, BRK };
	  s1[0] = 1;  run(p, sizeof p);  expect_wst("pop", s1, 1); }

	/* Popping the only item leaves nothing, which is the case that used to
	   read whatever the wrapped pointer landed on. */
	{ Uint8 p[] = { LIT, 7, POP, BRK };
	  run(p, sizeof p);  expect_wst("pop to empty", s1, 0); }

	{ Uint8 p[] = { LIT, 7, INC, BRK };
	  s1[0] = 8;  run(p, sizeof p);  expect_wst("inc", s1, 1); }

	/* ------------------------------------------------------ keep mode */
	{ Uint8 p[] = { LIT, 5, LIT, 3, ADDk, BRK };
	  s3[0] = 8; s3[1] = 3; s3[2] = 5;
	  run(p, sizeof p);  expect_wst("addk keeps operands", s3, 3); }

	/* ----------------------------------------------------- return mode
	   STH moves the top of the working stack to the return stack. ADDr then
	   adds the two return-stack values (3 on top, so 5 + 3) and STHr brings
	   the result back to the working stack. */
	{ Uint8 p[] = { LIT, 5, STH, LIT, 3, STH, ADDr, STHr, BRK };
	  s1[0] = 8;  run(p, sizeof p);  expect_wst("return-mode add", s1, 1); }

	/* ---------------------------------------------------- zero page */
	{ Uint8 p[] = { LIT, 0x42, LIT, 0x10, STZ, LIT, 0x10, LDZ, BRK };
	  s1[0] = 0x42; run(p, sizeof p); expect_wst("stz/ldz", s1, 1); }

	/* Zero-page short access wraps at 256. STZ2 pops the index from the top,
	   so the value goes down first and the index last. Writing a short at
	   0xff puts the low byte at 0xff and the high byte at 0x00. */
	{ Uint8 p[] = { LIT, 0x11, LIT, 0x22, LIT, 0xff, STZ2, BRK };
	  run(p, sizeof p);
	  expect_u8("zp low byte at 0xff", dux.mem[0xff], 0x11);
	  expect_u8("zp short wraps to 0x00", dux.mem[0x00], 0x22); }

	/* A plain short round trip through the zero page.

	   Conventions, all verified against the core:
	     - LIT2 input is high byte first: "LIT2 12 34" is 0x1234
	     - on the stack the LOW byte sits on top
	     - memory is big-endian: the high byte goes at the lower address
	   So pushing 0x11 then 0x22 builds 0x1122, stored as mem[0x40]=0x11
	   (high) and mem[0x41]=0x22 (low), and reads back as 0x1122. */
	{ Uint8 p[] = { LIT, 0x11, LIT, 0x22, LIT, 0x40, STZ2,
	                LIT, 0x40, LDZ2, BRK };
	  run(p, sizeof p);
	  expect_u8("zp short high byte at low address", dux.mem[0x40], 0x11);
	  expect_u8("zp short low byte at high address", dux.mem[0x41], 0x22);
	  expect_short("zp short round-trip", 0x1122); }

	/* ------------------------------------------------------- long mem
	   Memory ops pop the address from the top, so push the value first,
	   then the address. In short mode both operands are shorts, so the
	   value must be pushed as a LIT2 short too. */
	{ Uint8 p[] = { LIT2, 0x00, 0x77, LIT2, 0x03, 0x00, STA2,
	                LIT2, 0x03, 0x00, LDA2, BRK };
	  run(p, sizeof p);
	  expect_u8("sta2 high byte", dux.mem[0x0300], 0x00);
	  expect_u8("sta2 low byte", dux.mem[0x0301], 0x77);
	  expect_short("lda2 read back", 0x0077); }

	/* Store a full 16-bit value and read it back intact. */
	{ Uint8 p[] = { LIT2, 0x12, 0x34, LIT2, 0x04, 0x00, STA2,
	                LIT2, 0x04, 0x00, LDA2, BRK };
	  run(p, sizeof p);
	  expect_u8("sta2 high byte", dux.mem[0x0400], 0x12);
	  expect_u8("sta2 low byte", dux.mem[0x0401], 0x34);
	  expect_short("sta2/lda2 16-bit round-trip", 0x1234); }

	/* -------------------------------------------------- circular wrap */
	/* 300 pushes starting from an empty stack must wrap rather than corrupt
	   memory. The pointer ends at 300 & 0xff = 44. */
	{ Uint8 p[601];
	  int i;
	  for(i = 0; i < 300; i++) {
	  	  p[i * 2]     = LIT;
	  	  p[i * 2 + 1] = (Uint8)i;
	  }
	  p[600] = BRK;
	  run(p, 601);
	  expect_u8("working stack wrapped", dux.wstp, (Uint8)(300 & 0xff));
	  /* The 300th value pushed should still be readable through the wrap. */
	  expect_u8("value below wrap intact",
	            dux.wst[(Uint8)(dux.wstp - 1)], (Uint8)299); }

	/* Underflow: popping an empty stack wraps the pointer to 255. */
	{ Uint8 p[] = { POP, BRK };
	  run(p, sizeof p);
	  expect_u8("underflow wraps", dux.wstp, 255); }

	/* ----------------------------------------------- calls and returns
	   JSR stashes the return address and jumps relatively. Returning needs
	   JMP2r2 (0x6c), the SHORT-mode form: the stashed address is a full
	   16-bit value, so a byte-mode JMP2r would only consume one byte of it
	   and jump relative. Padding is INC rather than BRK so that falling
	   through the return path would be visible in the stack pointer. */
	{ Uint8 p[] = { LIT, 8,          JSR,     /* 0,1,2 */
	                BRK,                     /* 3: return target */
	                INC, INC, INC, INC,     /* 4..7 */
	                INC, INC, INC,          /* 8..10 */
	                JMP2r2 };               /* 11 */
	  run(p, sizeof p);
	  expect_u8("jsr consumed the target", dux.wstp, 0);
	  expect_u8("jsr/jmp2r2 balanced", dux.rstp, 0); }

	/* A genuinely nested pair. Addresses are absolute, so LIT2 02 1b is
	   0x021b: offset 0 calls offset 27, which returns to offset 13, which
	   jumps back to offset 4 where the outer call lands on BRK. */
	{ Uint8 p[] = { LIT2, 0x02, 0x1b,   /*  0: address of offset 27 */
	                JSR2,               /*  3: call inner           */
	                BRK,                 /*  4: outer return target  */
	                LIT2, 0x02, 0x1b,   /*  5: address of offset 27 */
	                JSR2,               /*  8: call inner           */
	                LIT2, 0x02, 0x04,   /*  9: address of offset 4  */
	                JMP2,               /* 12: return to outer      */
	                INC, INC, INC, INC, /* 13..16 */
	                INC, INC, INC, INC, /* 17..20 */
	                INC, INC, INC, INC, /* 21..24 */
	                INC, INC,           /* 25..26 */
	                JMP2r2 };           /* 27: inner return         */
	  run(p, sizeof p);
	  expect_u8("nested jsr balanced", dux.rstp, 0);
	  expect_u8("nested jsr consumed targets", dux.wstp, 0); }

	/* ------------------------------------------------------------ video */
	dux_reset();
	dux_fb_clear(0);
	dux_fb_pixel(0, 0, 3);
	dux_fb_pixel(3, 0, 1);
	dux_fb_pixel(4, 0, 2);       /* crosses a byte boundary */
	expect_u8("fb pixel 0,0", dux_fb_get(0, 0), 3);
	expect_u8("fb pixel 3,0", dux_fb_get(3, 0), 1);
	expect_u8("fb pixel 4,0", dux_fb_get(4, 0), 2);
	expect_u8("fb last pixel",  dux_fb_get(319, 199), 0);
	/* The framebuffer must sit inside VRAM and not spill past it. */
	expect_u16("fb start", (Uint16)(FB_BASE + FB_BYTES - 1), 0xbe7f);

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}