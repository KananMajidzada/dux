wip/ - programs that are not ROMs
==================================

Nothing in this directory ships. A program earns a place in `asm/` and then in
`roms/` by passing three checks, all run by `make verify`:

1. **It assembles.** `make roms` fails the build on a bad one.
2. **It draws what a prediction written down in advance says.** The count goes
   in `tools/expect.txt` *before* the ROM is run, and `tools/verify.sh` compares.
   A missing prediction is a failure, so nothing ships unchecked.
3. **It returns an empty stack after every frame**, and no two ROMs draw the same
   picture. `tools/balcheck.sh` and `tools/distinct.py` cover those.

Layout
------

    (hash.tal is gone from here.) It shipped as `asm/hash.tal`. It drew 1490
                  dots where the prediction said 3000, and the reason was not in
                  it at all: `@clear` in the prelude erases with a sprite, and it
                  asked for a thirty-two-pixel run from a port that tops out at
                  sixteen. See NOTES.md.

    probe12.tal   ISA probes. These draw nothing on purpose: they print a value to
    probe14.tal   the screen as a pixel position so a number can be read off the
    probeA.tal    picture. They are diagnostics, not programs.
    probeB.tal

The subdirectories
------------------

    shipped/      56 programs that were retired out of `asm/` rather than
                  deleted. Kept because these copies are the *earlier* drafts -
                  several of them were broken in ways the shipped ones are not -
                  and because the diff between the two is the shortest record of
                  what each fix was. To rebuild one, move its `.tal` back into
                  `asm/` and run `make roms`.

                  Two of the five programs now in `asm/` are *not* here, because
                  they were rewritten in place rather than replaced. `bounce.tal`
                  was the bouncing square - twenty-five pixels of white, colour 1,
                  which is what its prediction said for a long time - and became
                  the DVD logo on request. The old source was not copied aside
                  first, so it is gone; what it was is recorded in NOTES.md and in
                  the history of `tools/expect.txt`. `analog.tal` is new.

                  The lesson is the same one as the `rm` below: a program being
                  *replaced* deserves the same `mv` as a program being *pruned*.

    drafts/       earlier drafts of programs that were fixed in place. Superseded
                  by what is in `asm/`.

    superseded/   files byte-for-byte identical to a shipped program. Moved rather
                  than deleted so the duplicate is on the record: two identical
                  programs satisfy the same prediction, which is exactly the
                  defect a prediction table cannot see.

Recovering a program
--------------------

The blocker is almost always the same one this project kept hitting:

- `INCLUDE "common.tal"` resolves *next to the file that asks for it*. A program
  in `wip/` therefore cannot see `asm/common.tal` and fails with `cannot open`
  before assembling an instruction. Copy the prelude across, or assemble from
  `asm/`.
- A colour is pushed with `LIT $xx` and then handed to `@plotat`, which pops its
  own argument. The byte is left behind and the picture comes out blank or
  wrong. Leave it on the stack.
- `EQU2` pushes one *on* equality, so `EQU2 / JNZ skip` skips the matching case.
  `NEQ2` is what a jump-to-skip wants.
- `GTH2` tests the first value pushed against the second, so `LDA2 x / LIT2 k /
  GTH2 / JNZ` means "x > k" and the count belongs *below* the bound.
- `STA2 $zp $value` stores a **short**, whose high byte lands at the lower
  address. A flag that is one byte must be stored with `STZ`.
- A sprite's length is `(val >> 5 & 3) + 1` **bytes**, so the widest run the port
  can be asked for is four bytes and therefore sixteen pixels. An erase loop that
  steps by thirty-two stamps half of every block.

Each of those cost real time here, and each is written down in NOTES.md next to
the measurement that established it.

## The sixth, and the one that hides best

An inverted `JNZ` on a comparison. `GTH2` sets its flag when the comparison is
true and `JNZ` takes the branch when that flag is set, so

    LDA2 x
    LIT2 5
    LTH2                    ; is it against the wall?
    JNZ @check-the-other-wall

jumps to the other-wall test exactly when the block *is* against this one. The
program keeps running, keeps its stack even, and draws the pixels it was always
going to draw - just all in the wrong place, or not at all. `bounce` sat frozen
against the left wall for three hundred frames drawing its exact twenty-five
pixels a frame and passed every check the build runs. `rays` rotated the wrong
vector and drew a dotted line.

The way to catch it is to print something that moves. A pixel count cannot.

Also, while adding to this list: `SUB2` is *first minus second*, so negating a
value means pushing the zero first. And the screen has three usable foreground
colours, not four - the fourth is the background and a pixel written in it is
simply lost.

Two more, both of which passed every check the build runs:

**The order of the arguments, which no stack checker can see.** `JSR2 @plot`
with five bytes on the stack is five bytes on the stack whichever order they are
in. `@plot` takes colour, y, x off the top, so the colour has to be pushed
*last*. Push it first and the row becomes the colour and the coordinates come out
of the colour. `moire` and `weave` both did this; `-X` was silent both times.

**A counter that is tested but never incremented.** A loop whose exit condition
compares a variable nothing moves is bounded in the source and unbounded in the
run. Every instruction is well formed and every stack is even, so there is
nothing for a static pass to say. `weave` drew its top row over and over until
the frame ran out of steps.

The check that does work, and is worth remembering from both of those: print
something that moves. A pixel count cannot tell a working figure from a frozen
one, and `bounce` sat against a wall for three hundred frames drawing its exact
twenty-five pixels a frame.

## Four more, from the DVD logo and the clock face

All four passed every check the build runs - clean assembly, a stack that stayed
even, a prediction that matched - and were found by looking at what came out.

**A counter that is tested and never advanced.** `@block3` counted its three
columns and never moved along them, so all nine pixels of every 3x3 block landed
on the same one. The wordmark came out as a dotted line, 153 pixels instead of
1011. The same mistake as `weave`'s, and the arithmetic was fine.

**A routine that writes to a cell the caller still needs.** `@hand` reads an inner
and an outer radius in turn and stored the outer over the cell the caller had put
the inner in. The *second* of twelve marks was drawn with both ends on the same
radius, so it was one pixel long: eleven dots round the rim instead of twelve
dashes, which reads as a design choice rather than a fault. `@offset` has its own
radius cell now. The general shape - a routine that reads two of the caller's
values should write to neither of them.

**A table of pairs indexed by entry.** Two bytes to an entry, so the index wants
doubling. Reading `base + index` is not out of range and not a crash: it reads a
good byte of the table, the wrong one. Every hand came out one entry behind, and
the clock face was still a clock face, showing a different time. There is no
`DUP2`, so the doubling is `base + index + index`.

**A short read as a byte.** `STA2 $32` with 76 in it leaves `$00` at `$32`, so
`LDZ $32` is zero. Every radius in the clock is under 256, so they are bytes
now. What made it expensive was not the size but the *shape* of the failure: the
probe I wrote to check the arithmetic shared the same mistake, so it agreed with
the broken ROM and told me nothing. A probe that reproduces the bug under test is
not evidence.

And the generalisation, which cost most of a day: **`JNZ` goes when the comparison
is true, so it cannot be used to skip its own result.** Branching the obvious way
round skips a step exactly when the step is wanted. In the line walker that made
every line a staircase running off at forty-five degrees; in the same walker's
other branch it made the error term stop changing, so the short axis moved once
and stopped and every diagonal fell short of its far end.

Two things that are not bugs but read like them, and both cost time:

  - `$0036` is fifty-four, not sixty, and the twelve-mark loop stopped past it, so
    the last mark was never drawn. The comment said sixty.
  - `LDZ $nn` reads zero-page `$nn`. `LDZ $01` is *the contents of address 1*,
    which is zero - not the number one. Two separate probes of mine pushed a
    colour of zero this way and drew nothing, and I read that as the ROM drawing
    nothing.

**Predict the picture, then render it.** `tools/clockmodel.py` draws the clock
face in Python from the same tables, the same division and the same line walk,
written from the assembly rather than from the picture. Over 101 times it agrees
with the ROM to the pixel *and to the colour of every pixel*, which caught four
defects a count could not: eleven marks instead of twelve, four diagonals a pixel
short, and the hour hand pointing at five times the time.

## Five more, from Pong

Every one passed assembly, the stack checker, a balanced stack, and a matching
prediction on every frame where nothing overlapped. All five were found by
comparing **whole frames** against a model of the rules rather than by counting
pixels.

**`LDZ $02` is the contents of zero-page address 2.** `LDZ` reads an address;
`LIT` pushes a number. Writing `LDZ $02` because the colour is 2 draws in colour
zero, which is invisible and present in every trace. Two sites: the centre line
and the ball.

**Three pushes before an `ADD2` compare the wrong pair.** `LDA2 x / LIT2 k / ADD2`
sums the two; a third push in between does not join the sum, it becomes the other
operand of the comparison. `bx + 18 + 3` against 18 is true for every ball on the
screen, so the player's paddle never returned the ball and the score ran away 5-0.
There is a scan for the shape now, run after every edit.

**A short at `$4e` owns `$4f`.** The paddle colour sat one past the short holding
the paddle's x, so the x's low half overwrote it every frame - 18, masked to two
bits, is colour 2 - and the player's paddle came out the same grey as the score.

**`EQU2` pushes one *on* equality, so `EQU2 / JNZ` jumps out of the match.** The
ball's "where on the paddle did it land" has three cases and the middle one has to
carry on as it was; written the natural way it jumped to the *above* case instead,
so a dead-centre hit came off at two pixels a frame instead of straight on.

**Positive means "above zero and below 32768."** A step of -3 is 65533 as a short,
so a single test against zero says *yes* for every ball going the other way.

And two that need input to see at all, which is the point:

**A short holds its high byte at the lower address, and the mouse pair is the
other way round.** `DEI` gives the low half of the mouse y, and that has to go one
byte *past* the base. The other way round reads the mouse as its value times 256.
The mouse starts at zero, so eleven frames matched the model exactly and then the
paddle appeared pinned to one end of the screen and swinging the wrong way.

**`duxemu` increments the mouse path before it reads the point**, so game frame 2
sees `path[1]`. One point out puts the paddle thirteen pixels out for a whole
sweep - and thirteen is half a paddle, so it reads as a game that is stiff rather
than wrong.

A parked mouse matched and a moving one did not. **A bug that only appears when
the input moves is not a bug the static checks can find**; the test that finds it
is to drive the input and compare against the model driven the same way.

**A file that includes another gets that file's zero page.** Re-declaring a name
an included file already owns does not make a new variable - it makes two owners of
one cell. Harmless until the day a routine that writes the shared cell is called
from the file that did not know it was sharing.

**Everything a frame draws must be in the erase list, and the erase sprite must be
the same height as the thing it erases.** A blinking colon that was drawn but never
erased stayed lit for ever; the erase that existed covered the digit boxes and not
the colons. And an erase one row tall against a two-row dot leaves a row behind -
eight pixels, two dots, two colons - which is a number small enough to look like
rounding.

**When a model says a verified ROM is wrong in a way the source does not suggest,
suspect the model.** `(code >> mask) & 1` instead of `code & mask` reported stale
pixels on sixty consecutive frames and sent me hunting for an erase bug that was
never there. A real erase bug produces extras *and* missing pixels together; a
uniform pile of extras only, from every digit at once, is the model being wrong.

**One-pixel-wide next to two-pixel-thick reads as a scratch.** A seven-segment
figure whose columns are one pixel and whose bars are two rows thick is as hard to
read as the reverse. Segments want the same thickness in both directions, and 2 and
5 are the digits that show it - they are the two whose shape is mostly column.

## blackjack, and the five ways it drew nothing

`wip/blackjack.tal` assembles clean, has a balanced stack, and does not work.
Only the four card outlines come out; no rank, no suit, no label, no total, no
message, no button. It is parked rather than shipped for that reason, and
`roms/` has no prediction for it because a prediction is a claim, not a
transcription of whatever came out.

What *is* finished and worth keeping: the deck of fifty-two as `rank * 4 + suit`
so the rank is a shift and the suit an AND, the Fisher-Yates shuffle, the rules
(ace eleven or one, dealer stops on seventeen, blackjack, bust, the dealer
peeking only on an ace or a ten up), and the totals. The fault is in the drawing.

Four mistakes, all the same shape, and all of them things the stack checker
cannot see because the depth comes out right:

**A routine that takes y on the stack and a caller that pushes it first.** The
store order in `@hline` is `STA2 ly / STA2 lx`, and `STA2` takes the *top*, so
the top has to be y - which means x is pushed first. `@box` pushes x then y and
the boxes come out right; `@cb-row` pushed y then x and the card back came out at
(12, 132) instead of (132, 12), with its arguments the wrong way round.

**`@widen` on something that is already a short.** It pops one byte and pushes
two, so the depth grows by one and stays consistent - and the value below it is
now the top byte of a number that was already whole. Four of these survived an
assembler that reported the program clean.

**Adding to x in place to get the far edge.** `@box` computed the right edge as
`x + width` into `x`, and then drew the left edge from `x` too, so both sides
landed in the same place. The far edge needs its own cell.

**A loop where a division would do.** `@fixup` turned each ace down by ten until
the hand fitted under twenty-one, and the loop never terminated: the ROM hung,
with no message, and the trace showed the total stuck at 22 while the store of 12
succeeded. Rewriting it as `k = (raw - 21 + 9) / 10` capped at the ace count
fixed it, and the loop-free version is better code anyway - one pass, no test to
get wrong.

**The lesson, and it is the same one as the digit fonts:** a routine that draws
something takes its arguments *pushed in a stated order*, and that order has to
be written down at both ends. `@plot` in common.tal takes x, y, colour; mine
took x, y, addr, colour; the two are indistinguishable at the call site unless
the contract is written on the routine and the caller's comment agrees with it.

## blackjack is abandoned

Not parked pending a fix - given up on. `wip/blackjack.tal` assembles clean and
has a balanced stack; it is here only so the finished parts are not written
twice: the deck as `rank * 4 + suit`, the Fisher-Yates shuffle, the rules, the
totals. It draws four card outlines and nothing else.

Two real bugs came out of it that are worth keeping, because both are invisible
to every check this project has:

**A routine that takes its arguments off the stack, and a caller that puts them
in cells first.** `@box` pops five; `@button` filled five cells and then called
it, so `@box` read an empty stack and plotted whatever the cells held - which is
where 2160 stray colour-0 pixels down the left-hand edge came from, and what
erased the labels and the message behind them. Every write was to a legal
address and every stack ended balanced.

**A zero-page map of one-byte cells, with shorts stored in it.** `STA2 $ZP.q2`
wrote two bytes into a cell whose neighbour was one byte along. The map now puts
bytes first and shorts on even boundaries, like common.tal's.
