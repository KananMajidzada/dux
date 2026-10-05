# Facts established by measurement

These were each settled by running the machine, not by reading the source.
Several contradict what the source reads like, which is why they are written
down.

## The compare instructions

`dux.c` reads `OPC(GTH, POx(a) POx(b), PU1(b > a))`, which suggests the flag
compares the top against the deeper value. It does not. Measured by branching on
the flag and plotting a pixel per test, so the answer is which pixels are lit and
not a number to misread:

    GTH with 5 then 3    FLAG SET      so GTH is  deeper > top
    GTH with 3 then 5    flag clear
    LTH with 5 then 3    flag clear    so LTH is  deeper < top
    LTH with 3 then 5    FLAG SET

`EQU` is symmetric and is true when the two are equal.

So the idiom is "test, then jump to the exit", which is what `paint.tal` and
`clock.tal` already do:

    LDZ2 $ZP.fc
    LIT2 $00c8
    GTH2
    JNZ @clear-done      ; jump out when fc is past the last row

Getting this backwards does not crash: it exits the loop at once, or never exits
it at all, and the symptom looks like a drawing bug rather than a compare bug.

## `SFT` and `SFT2` are different opcodes

`SFT` is a byte shift and `SFT2` a short one: they assemble to `1f` and `3f`.
`dux.c` lists one shift opcode and `POx` means "as wide as this instruction",
which reads at first like `SFT` always works on shorts. It does not, and the
disassembly settles it in one command.

(The stack checker is the other half of this: it reports the byte offset, so
`pops N from a stack M deep` names the instruction and how deep it was.)

## A byte where a short belongs is invisible, not loud

`DIV2` divides two shorts. Handed `LDZ $ZP.b / LIT2 $0002 / DIV2` it reads one
byte of value, one byte of whatever is under it, and divides that: no crash, no
stack warning, and a result that is usually zero. If that result is then used as
a palette index, zero is the background colour and the program appears to draw
nothing at all. Widening the byte first is the whole fix.

This is the same shape as the `STA $zp.x $00` mistake and it is the failure I
keep making, so it is worth stating as a rule: **any instruction that takes two
operands takes two shorts unless its name says otherwise.**

## Two operands, and the width comes from the name

`ADD SUB MUL DIV AND ORA EOR` take two operands and return one. So do
`EQU NEQ GTH LTH`.

Each has a byte form and a short form, and the name says which: `ADD` and `ADD2`,
`GTH` and `GTH2`. `dux.c` writes them as `POx(a) POx(b)`, where `POx` resolves
against the opcode's mode bit at runtime - so reading `dux.c` tells you the shape
but **not** the width. Reading `dux.c` to conclude "AND is a byte operation" is
how this note first got written wrong.

### `SFT2` takes a byte control and a short value - value pushed first

The control operand is one byte even in the short form, and it goes on *top*:

    LDZ2 $ZP.tmp      ; the value, a short
    LIT $06           ; the control, one byte
    SFT2              ; pops three, pushes two

Written the other way round - `LIT2 $0006 / SFT2` - it pops three from a stack
four deep and leaks, and the cube stops drawing entirely. Measured: 204 bytes
leaked and a blank screen, from a change that looked like a tidy-up.

`dux.c` says `OPC(SFT, PO1(a) POx(b), ...)`: a one-byte control, and the value
at the width of the mode. The order is value-then-control, which is the opposite
of how `LIT2`-then-`ADD2` reads.

## The cube's leak is inside `@fill` - established by test

Stubbing `@fill` out with a routine that consumes its argument shows **no leak at
all**. Stubbing it with one that does *not* consume it still shows twelve, which
is what made me clear the sort loop for a session. So the twelve bytes are two a
call, six calls a frame, and they are inside `@fill`.

## Bare `LDA` pushes ONE byte

Measured by balance: the stack is empty on entry, `LDA2` pushes an address, a
bare `LDA` pops it, and one `STA` follows. No warning, so the result is one byte.

This kills the theory I had been carrying for two sessions - that bare `LDA`
returns a short and `@facecolour` truncates it. It does not, `@facecolour` is
balanced, and adding the `POP`s I tried on that theory made things worse (12
bytes became 196).

So the leak is somewhere else in `@fill`. Ruled out by test so far: `@facecolour`
(balanced - bare `LDA` pushes one byte), `@shr6` (its `SFT2` is correct and
balanced), the bare `SFT` in the key-zeroing, and the sort loop itself.

**And it is in `@fill`'s own body, not in any of its callees.** Each of these was
replaced with a balanced stub - the right arity, so the stub adds no leak of its
own - and the twelve bytes survived all five:

| stubbed | leak |
|---|---|
| `@seti2` | 12 |
| `@getsx` + `@getsy` | 12 |
| `@line` (early return, body left intact) | 12 |
| `@facecolour` | 12 |
| `@fill` itself | **none** |

So it is in the 214 lines between `@fill`'s entry and its calls. Truncating
`@fill` at successive points does not isolate a line, because the leak is
**path-dependent**: `@fill` takes one of three paths depending on the quad's
shape, and stopping it early gives 36 bytes at some cut points and 12 at others.
That rules out a single stray instruction and points at something that runs a
different number of times on each path - a span count, most likely, since `@line`
is called once per span.

Stubbing callees is the technique that works here. Truncating the caller is not,
because it changes the thing being measured.

**A byte add on a short address, in `@fill`.** Three sites:

    LIT2 @faces
    ADD2          ; a two-byte address
    LIT $01       ; one byte
    ADD           ; takes the address's low half and the literal
    LDA           ; and reads from the result

Widening the literal to `LIT2 $0001 / ADD2` changes neither the leak nor a
single pixel of output - a 24-byte table at a fixed address is never crossed by
`j*32` for j = 0 to 5, so the truncated address is the right one anyway. It is a
real latent bug and it is not this leak. The same shape in `@face-depths` was
*not* harmless and did change the picture; whether it bites depends entirely on
where the table sits.

## Bare `LDA` pushes ONE byte (older note)

`LDA` with no operand pops an address and pushes a single byte read from it.
Adding `POP`s to "drop the high half" after it underflows the stack - measured:
the leak went from 12 bytes to 196. `LDA2` is the form that reads a short.

A short pushed by `LIT2` has its **low byte on top**: `LIT2 $1234 / STA a / STA b`
leaves `a` = 52 and `b` = 18.

### `GTH2` is `top > deeper`; `LTH2` is `top < deeper`

**Measured, and the opposite of what this file said before, and the opposite of
how `dux.c` reads.** A probe pushed two literals and plotted a marker pixel
saying which way each comparison went:

| operands pushed | comparison | flag |
|---|---|---|
| 5 then 10 | `LTH2` | 0 |
| 10 then 5 | `GTH2` | 0 |

Read that as: with the first literal pushed first, it is the *deeper* operand
and the second is the *top*. So `LTH2` was false for 5 < 10 and `GTH2` false for
10 > 5 - which is only consistent if the comparison puts the top on the left.
`LTH2` is `top < deeper`, `GTH2` is `top > deeper`, and they are complements of
each other as they should be.

This is why the loop idiom reads the way it does: `LDA2 $ZP.si / LIT2 $0140 /
GTH2 / JNZ @done` is true exactly while the column is short of 320, because the
320 is on top. Getting the direction wrong does not crash - it inverts a loop
exit and the program draws something else entirely. `rings` kept the *largest*
depth instead of the smallest for exactly this reason and drew the crossed arms
of a box instead of its outlines: 16996 pixels where 3528 were predicted, and a
perfectly plausible-looking picture.

### `GTH2` and `LTH2` push ONE byte, and `JNZ` sees it

`dux.c` is right: `PU1`, a single byte flag. `JNZ` branches on it correctly -
confirmed by the same probe.

This note previously said the opposite twice, once claiming the flag was
invisible to `JNZ` and once claiming two bytes were left behind. Both were
read off `duxasm`'s "reached with 1 byte on the stack and with 3", which is an
ambiguity report about a join, not a statement about instruction width. The
width was settled by building a hundred-iteration loop round the idiom and
watching the stack: it does not drift.

What actually went wrong in `frame` was nothing to do with the stack. Four tests
all jumped to the plot, and the fallthrough *also* plotted - so the interior was
plotted as well and the border came out as a filled rectangle with a notch. A
predicate that is right about which way round it goes has to have one path that
draws and one that skips, and here both drew.

## Branching on a short leaves half of it behind

Short *arithmetic* - `ADD2 AND2 MUL2 SUB2 DIV2 ORA2 EOR2` - pushes a short.
Short *comparisons* - `GTH2 LTH2 EQU` - push a single byte flag. That asymmetry
is in `dux.c` and it is easy to miss, because both are "short forms".

So `AND2 / JNZ @skip` eats the low half as the condition and abandons the high
half, once a pixel. `@toflag` in `asm/common.tal` narrows one to the other:

    LDA2 $ZP.si
    LIT2 $000f
    AND2
    JSR2 @toflag
    JNZ @skip

`grid` leaked 42 bytes and `dotgrid` 12, both from exactly this, and both now
verify clean. Anything that branches on a short result needs it; anything that
branches on a comparison does not.

`cube.tal` has no such site - the same patch found nothing - so its 12 bytes are
still unaccounted for, and `@facecolour` is balanced. Stubbing `@fill` out is
the next test and it has still not been run.

## A byte index cannot be added to a short address

The single worst bug on this project, and it is silent every time.

```
        LIT2 @faces
        ADD2              ; two-byte address
        LDZ $ZP.i2
        ADD               ; byte add: consumes the address's low half and the
                           ; index, and orphans the address's high half
        LDA               ; reads from a truncated address
```

`ADD2 / LDZ / JSR2 @wide / ADD2` is the correct form. The wrong one assembles
clean, is stack balanced, lands in bounds, and returns *a* number - so a lookup
succeeds while reading the wrong table.

In `cube.tal` this made every one of the four corner depths of every face come
from a truncated address, which left all six sort keys zero, which left the sort
with nothing to order, which repainted face zero six times. The cube came out as
one flat silhouette. Two earlier diagnoses of it were wrong: the face colouring
was blamed on the palette and then on the colour comparison, and both "fixes"
moved a number without moving the picture.

## What is actually verified in cube.rom, and what is not

Measured, not read off the source:

- The six face keys are `130,194,192,192,194,194` - exactly the C model's.
- The sort visits faces in the order `1,4,5,2,3,0`, which is those keys
  descending with the painted face zeroed each pass. Correct painter's order.
- The rendered cube is 8490 pixels in a 99x98 box: a solid silhouette with no
  gaps and no spikes.

Not working: **every face is drawn in the same colour.** The keys, the sort and
the colour table are each individually correct when forced - setting `@coltab`
to a constant does change the whole cube to that colour, so the colour *does*
reach the plot - but the index into `@coltab` comes out the same for every face.
`@facecolour` reads the low byte of a short from `@faces + face*4`, which should
be `0,1,2,3,4,0` for the six faces, and those six values give three distinct
entries of `@coltab`. They do not appear. Still open.

## Find the background as the majority colour, never by sampling a corner

`tools/ppmcheck.py` is the only way to check a rendered frame in this project.
Use it, and predict the count before rendering.

The part that matters: the background is **the most common colour in the frame**.
A drawing that reaches a corner - which is what "drawn flush to the origin" means
- makes a corner sample land on the drawing, so the background is read off a
checker and every background pixel then counts as lit. A correct 6400-pixel
drawing reports as 63360 lit covering the whole screen, which reads exactly like
a runaway loop.

`checker.tal` was binned twice on that false report, and both times the "fix"
(a colour change, then a comparison instead of a mask) moved the number without
making it right. Two red herrings generated by a broken ruler. Draw with a margin
so the corner is background, and take the background as the majority so the
margin stops being load-bearing.

Related: the count is of the **union**, so a prediction must not double count
overlapping pixels. 100 cells of 64 pixels is 6400 only if no two cells touch.

## Open: byte `MUL`, `ADD`, `AND` and `EQU` are not behaving

Four ROMs written this round all failed together - `zigzag`, `sweep`, `hash`,
`checkera` - and all four use one of: byte `MUL`, byte `ADD`, byte `AND`, or
`EQU` on two shorts. `sweep` and `checkera` use only the last of those plus
short arithmetic, and both drew nothing at all. `zigzag` drew 400 pixels where
320 were predicted and leaked 108 bytes; `hash` drew nothing and leaked 238.

None of the other thirty-odd lines in this project use these, which is why
nothing caught them. Every ROM that works is built from `LDA2`/`LIT2`/`ADD2` and
the `GTH2`/`LTH2`/`EQU` comparisons, and the byte arithmetic forms have never
been exercised by anything that works.

So this is a gap in what has been *measured*, not a set of conclusions. What is
known: the four above fail, they fail together, and they share those
instructions. What is not known: which one, or why. Next step is a probe of the
four in isolation, the way the comparison probe settled the comparisons - one
marker pixel per instruction, saying what each returned.

## The comparisons, measured (probe12)

This is the version to trust. `LDA2 x / LDA2 y / OP2` with values pushed in that
order - so `x` is the deeper operand and `y` the one on top:

| operands | result | |
|---|---|---|
| si=10, sj=5 | `GTH2` -> 1 | deeper > top |
| si=10, sj=5 | `LTH2` -> 0 | deeper < top |
| si=5, sj=10 | `GTH2` -> 0 | |
| si=5, sj=10 | `LTH2` -> 1 | |
| `LIT2 $00a0 / LDA2 $ZP.si / SUB2` | 155 | deeper minus top |
| `LIT2 $00a0 / LDA2 $ZP.si / ADD2` | 165 | |

No stack warning, so the comparisons push one byte and the arithmetic a short -
the same asymmetry as the byte forms.

`probe9` said the opposite and was wrong: it leaked sixteen bytes and its
structure was suspect. It was read anyway, and then retracted, and the retraction
was based on it too. **Three separate claims about comparison direction have now
come from two probes and only one measurement.** The table above is the
measurement.

`GTH2` = deeper > top is what every working ROM already assumed, so nothing
shipped was wrong. What it does explain is `vshape`: I read its 18500 as a
failure, counted the same shape by column, got 25760, and "corrected" a correct
program down to 1640. The prediction was wrong, not the ROM - the fourth time on
this project, after `blocks`, `quad` and `diag`.

## Two of the six "cluster" failures were a bug in my generator

`hstripe` and `vstripe` were written by a helper that took the predicate test as
the whole body and never appended the two instructions that actually draw. Both
compiled, both ran, and both drew nothing - and I filed them with five other
programs as a mysterious ISA problem. `diff` against the working `stripes`
showed the missing lines immediately.

Before blaming the machine for a program that was never asked to draw anything,
check that it draws anything. Six programs failing "the same way" is evidence
about the generator until it isn't.

What is left unexplained: `stripes2`, `stripes3` and `hstripe` all measure
**30720** where a simulation says 33280, and 30720 is 96 rows rather than 104 -
the last eight rows of the screen never light. `blocks` uses the same test on the
same counter, with one more test in front of it, and gives the full 104. That is
still unexplained.

## `AND2` returns a SHORT, and `tools/stacksim.py` is calibrated not argued

`ADD2 MUL2 SUB2 DIV2 AND2 ORA2 EOR2` pop two shorts and return one short:
**net -2**. The comparisons (`GTH2 LTH2 EQU2 NEQ2`) return a single byte flag and
net -3.

I twice derived `AND2 = -3` by argument - most recently from `@cos` passing a
short to `@sin` and returning one, which only works if `@sin` is not +2. I took
the conclusion anyway both times.

**Calibrating against the ROMs the host already calls balanced settles it: `-2`
predicts zero for 21 of 31, `-3` for 13, `-1` for 13.** Agreement with known-good
programs beats an argument about one routine. That is now the method for this
whole file: derive a candidate, then demand the candidate reproduce every
program already known to be right.

The rule that survives is about *branching*, not width - `LDA2 x / AND2 / LIT2 k
/ GTH2` is balanced while `... / GTH2 / JNZ` is not. **Mask, then compare; never
mask, then branch.**

With `@plot = -5` (it *consumes* x, y and the colour - five bytes) the model sits
at 21 of 31, and the residuals fall into two clean clusters:

- `diag` `frame` `grid` `quad` `wedge` `sweep` all at exactly **-4**
- `bars` `dots` at -19, `checker` at -24

Six ROMs agreeing to the byte is one shared construct, not six coincidences.
And the discriminator is already known: **all six call `@plot` directly**, with
`LDA2 / STA2 $ZP.rx / LDA2 / STA2 $ZP.ry / LDZ2 / LDZ2 / LDZ / JSR2 @plot`,
while the 21 correct ones use the `@plotat` wrapper. So the error is in how that
explicit sequence is modelled, not in `@plot`'s own width - it is 4 bytes out
across six programs that share those seven instructions.

**And the plot sequence itself is correct.** Traced with `@plot = -5`:

    LIT $01      +1      LDA2 si  +2     LDZ2 rx  +2
    STA $ZP.t    -1      STA2 rx  -2     LDZ2 ry  +2
    LDA2 sj      +2      STA2 ry  -2     LDZ  t   +1
    JSR2 @plot   -5      ...            total    0

Ten instructions, net zero, in every ROM that uses the explicit form. So the -4
is in the **predicate**, and all six of those ROMs have one. The twenty-one that
predict zero reach `@plotat` through a wrapper instead, so whatever the predicate
does differently is the whole of the remaining gap on them.

**It was neither. `LDZ2` was missing from the width table entirely**, scoring zero
instead of +2, and there are exactly two `LDZ2` lines per plot site. That single
omission *was* the -4 cluster, and the model went from 21 to **25 of 31**.

Found by tracing one ROM line by line and reading the running depth - the only
thing that could have caught it, since the totals looked like six independent
errors and were one.

**Then `@wide`/`@widen` was wrong by 4 per call: it is +1, not -3.** It consumes
one byte and returns a short, so pushed-minus-popped is +1. `sweep` calls it
exactly once and its residual was exactly -4.

The model now agrees with the host on **28 of 31**. Still wrong: `bars` at -3,
`dots` at -3, `checker` at -4 - all three the multi-colour ROMs, which plot
`LDZ2 rx / LDZ2 ry / LDZ <colour> / JSR2 @plot` instead of going through
`@plotat`. That sequence is zero on its own, so the error is in whatever those
three share besides it.

`cube` reads +147 with `@divn` and `@sdiv` unmeasured, so it is **still not
usable on the ROMs that leak** - which remains the tool's whole reason to exist.
But the method is now settled and short: **trace one ROM, read the running depth,
and let the first instruction that should be zero and isn't name the opcode.**
Four sessions ago that method was an idea; it is now worth 17 ROMs of agreement
in a single afternoon.

## A count that matches can still be the wrong count

`diag.tal` plots where `(x + y) mod 16` is under four, and skips the rest. It
shipped at exactly the predicted 16000 pixels - and was drawing the *other*
240 pixels a row, leaving the eighty that should have been lit sitting in the
background colour. The total came to 16000 either way, because the unlit stripes
and the lit stripes are the same size.

The only thing that caught it was printing the whole histogram rather than the
count: the 16000 lit pixels were the background colour, which no drawing should
ever plot. So a prediction is worth having and is not worth trusting on its own -
check that the lit pixels are a colour the program actually asked for.

## Predict before you render

A predicted count is a bug report that arrives before you have looked at
anything. It says how many times a loop ran, which is the one fact a picture
never tells you directly.

The failures on this project were all found this way and none by looking: 400
against a predicted 1600 said three of four bands were missing; 3200 against
6400 said exactly half the board was in the background colour; and `10,10,8,8`
from a counter probe said all four loops were right at the moment the picture
said they were not.

Loop counters end on their exit value, so a loop that runs ten times prints ten.
Predict the *exit* value, not the last value used.

## `tools/stacksim.py` — calibrated on all 30 balanced ROMs

`duxasm` links a stack checker and is **silent on both ROMs that actually leak**
(`cube`, 12 bytes; `clock`, 60). `tools/stacksim.py` is the alternative: read the
source and add up. **It now predicts exactly zero for all 30 ROMs the host calls
balanced.** `cube` is the only disagreement, and it is one of the two that leak.

It was built by **calibration, not by reading `dux.c`**: derive a candidate
width, then require it to reproduce every program already known to be right.
That method found errors that reading the source had not:

| what | wrong | right | how found |
|---|---|---|---|
| `JSR2` | net 0 | per callee | 18 of 18 ROMs disagreed |
| `JNZ` | −3 | **−1** (supplies its own 2-byte address) | calibration |
| `@plotat`, `@toflag` | +1 | **−1** | calibration |
| `AND2` | −3 | **−2** | calibration |
| `LDZ2` | *absent* | **+2** | traced a ROM, read the running depth |
| `@wide`/`@widen` | −3 | **+1** (consumes 1, returns a short) | traced |
| `STA $nn $00` | −1 | **0** (immediate: pushes then pops) | traced |

Two lessons worth keeping:

- **Truncate, don't reason.** The last four errors each *looked* like several
  independent bugs — six ROMs sharing a −4, three sharing a −3 — and each was one
  fact. Tracing one ROM line by line and reading the running depth names the
  instruction at once; reasoning about one routine does not.
- **Five conclusions reached by reasoning from a single routine were wrong**,
  every time: comparison direction, `GTH2`'s width, bare `LDA`'s width, `AND2`'s
  return type, `@plot`'s sign. Not one measurement has been wrong.

Remaining: `cube` reads +151 with `@divn` and `@sdiv` unmeasured. Since the
model is now right on all 30 others, that +151 is either a real imbalance or one
more unmeasured construct in a ROM nobody else uses — and it is the one ROM where
a tool that can read it would matter.

## cube's imbalance localises to three routines

With `@divn`/`@sdiv` measured (both 0 — the same shape as `@shr6`), the
per-routine net for `cube.tal` is:

| routine | net |
|---|---|
| `@turn` | **+55** |
| `@project` | +13 |
| `@fill` | **-8** |
| `@getsx`, `@getsy` | +4 each, where +2 is expected |

So the residual is not spread across 1600 lines: it is one dominant routine and
two smaller ones. `@turn` alone is more than half of it, and the simulator can
name the routine without executing anything.

`@fill` at -8 is a different shape from the +12 the host measures, and that is
the useful part: a *static* net counts call sites, while a leak counts calls.
`@fill` is called six times a frame and nothing else in the project is, so -8
per site is consistent with a small per-call leak once the loop bodies are
accounted for. That is exactly why the balanced-stub experiments on `@fill` kept
returning to the same number whatever I removed from it.

`@getsx`/`@getsy` netting +4 where the caller expects +2 is the most likely
single missing width, and it is worth one look: both push a short, so either one
of them pushes two, or one of the two has an instruction the model has wrong.

## Bare `LDA2` nets 0; `LDA2 $nn` nets +2

They are the same opcode with different widths, and reading the source gives no
hint of which is which: **`LDA2 $ZP.si` pushes a short, but a bare `LDA2` pops a
short address and pushes a short, for a net of zero.**

The model treated both as +2. It cost 14 bytes on `cube` (+91 to +77) and nothing
anywhere else, because the thirty other ROMs only ever use `LDA2 $nn` - they
reach the device through `@plot`, which never takes a bare `LDA2`.

**This is the general lesson of the whole tool: a mnemonic's width depends on its
form, not its name.** `STA $nn` and `STA $nn $00` differ by a byte, `LDA2` with
and without an operand differ by two, and every one of those was wrong here at
some point.

## The model predicts leaks, not just balance

Pointed at `clock.tal` - which leaks **60 bytes** and which I had never analysed
- it predicts **+50**. Same sign, same order of magnitude, with ten callees still
unmeasured that plausibly account for the difference.

That is the first evidence the simulator works for its actual purpose. Until now
it had only been shown to say zero about programs the host also calls balanced,
which is a much weaker claim: a model that always says zero would score 30 of 30.
Producing a positive number of roughly the right size on a program that leaks is
a different and much better result.

So the +77 it reports for `cube` against the host's 12 is **not** a disagreement
of the same kind. The likely explanation is the per-site/per-call distinction:
`@fill` is called six times a frame and nothing else in the project is, and a
static net cannot weight that.

**The ten unmeasured `clock` callees are the next thing worth measuring** -
`@seg-h`, `@seg-v`, `@draw-seg`, `@erase-all`, `@erase-one`, `@draw-one`,
`@draw-colons`, `@colon`, `@pair`, `@ds-set`. If the prediction then lands on 60
rather than 50, the difference between the two numbers is the leak, named.

## Internal labels break "measure each routine" - a trap worth writing down

`clock.tal`'s ten unmeasured callees (`@seg-h`, `@seg-v`, `@erase-all`, ...) look
like routines. Splitting the file on `^@` to measure them one at a time gives
numbers that are **per label-block, not per routine**, because `@colon`,
`@draw-one` and `@erase-one` are branch *targets inside* their callers. Applying
the result made the prediction worse: +50 became +22.

A routine is not the text between two labels; it is a call graph. Measuring one
means starting from a call site and following its `JMP2r2`, not from a label.
Every other number in the file is a label-block net and is fine, because they
were never summed - only read for the one that stood out.

## clock.rom: three bugs, one of them invisible in the disassembly

The 60-byte leak was three separate faults, and finding them needed the sprite
device to report what it was asked to draw.

**1. `AND2` leaves a short; `JNZ` reads one byte.** In `@draw-colons`,
`LDZ2 frames / LIT2 $0020 / AND2 / JNZ` pushed a short and tested it with an
instruction that consumes a single byte of flag. The comparison has to narrow it
first, so `LIT2 $0020 / EQU2` goes in between: EQU2 pushes the one byte JNZ
wants. Found by searching the whole project for the pattern - it occurred in
exactly two places, both in clock.tal.

**2. A short stores its high byte at the *lower* address.** `@seg-h` and
`@seg-v` chose the segment kind with `STA2 $ZP.fl $0000` / `$0001`, so the one
landed in the *high* half and `$2c` held zero. Loading it back with `LDZ $ZP.fl`
therefore always read zero, every vertical segment took the horizontal branch,
and **a 2 rendered as a 2 with a tail** - the exact symptom the comment on
`@draw-seg` blamed on mask ordering. The flag is one byte and is now stored and
loaded as one byte, with `STZ`.

**3. `GTH2` is second-from-top > top.** The erase loop read
`LDZ2 py / LIT2 $0008 / GTH2 / JNZ done`, which tests `8 > py`, so it stopped
after a single row and left a stale pixel from the previous digit's right-hand
column. The count belongs on top and one past the last row: `LIT2 $0009`.
That also pins the direction NOTES had recorded as never established - `dux.c`
reads `PU1(b > a)` with `a` popped first, so the *deeper* operand is `a` and the
comparison is `b > a`.

**And a fourth, found only by looking at the picture:** `@draw-colons` pushed
`LIT2 $00b4` and returned without calling `@colon`, so only one of the two
colons was ever drawn and the pushed address was left behind.

### Verifying a clock

A clock reads live time, so its pixel count changes with the minute and no
recorded prediction can hold. `DUX_TIME=<epoch>` pins the Datetime device
(clock.c); `tools/expect.txt` carries the epoch as field 5 and `verify.sh`
exports it for that one ROM only.

The prediction is made from the segment table rather than read off a render:
bars are 5 px, a vertical is two flip-y calls of 4 px, and the six digits of
12:34:00 are 1, 2, 3, 4, 0, 0 - `1`=$06, `2`=$5b, `3`=$4f, `4`=$66, `0`=$3f -
which is 123 px of digits plus 4 px of colons, **103 after the erase clears the
one overlapping pixel**. It matched on the first run.

## tools/routinegraph.py: a routine is a call graph, not a label

Splitting the source on `^@` and summing each block is wrong, because labels are
not routines - `@colon`, `@draw-one` and `@erase-one` are branch targets inside
their callers. Applying those numbers made clock's prediction *worse* (+50 to
+22).

Two further corrections were needed before the walk meant anything:

- `JNZ` is **conditional**. Treating every branch as unconditional skips the
  fall-through, which is half of every dispatch chain; @draw-one is seven of them.
- `JSR2` **returns**, so the instruction after a call has to be walked too.
- And the net has to be a **path sum**, not a sum over reachable instructions.
  `@abs2` is +0 down either path and came out as +2 by having both arms added
  together. The tool reports the *set* of nets a routine can reach, so a
  path-dependent routine is visible rather than averaged away.

On that footing clock reads **+0 on every path**, and cube's `@reset` reads
{+9, +11, +12, ... +22} - the host's 12 is in the set, and the minimum, +9, is
what every frame pays.

## Per-call stack tracing in the VM: two failures, and why

The idea was to have each JSR2 record its depth and each JMP2r2 compare, which
would name the exact leaking call site - something no static pass can do, since a
pass counts a site once and a loop body is a site. Both obvious measurements are
wrong:

- **Against the depth at the call**: a caller pushes its arguments before the
  target literal, so a routine that consumes exactly what it was handed returns
  far below where it started. `@put16` is given a value and a port and correctly
  hands back nothing; that reads as a five-byte leak. Every good routine looks
  broken.
- **Against the previous return at the same site**: that measures the caller's
  argument discipline as much as the callee's. `@abs2` nets exactly zero on both
  paths and still drifts, because its callers reach it with different amounts on
  the stack from one loop pass to the next. On cube it reported **921 spurious
  drifts** and buried the real one.

So it was removed. What survives is the host's own check - the vector must end
balanced - which is coarser but is a fact about the whole run rather than an
inference about one call. `DUX_TRACE_SPRITE` stayed, because it reports fields
the device is about to use rather than inferring intent, and it is what turned
"verticals are wrong" into "`@ds-vert` is never reached".

## The static tools' standing limit

`stacksim.py` sums reachable instructions and so cannot see a diamond:
it reports cube at +75 where the host measures 12. `routinegraph.py` does a
proper path sum and reports {+9..+22}, which contains the truth but is an upper
set rather than a number. Neither is a leak detector. The host's balance check
is the detector; the static tools narrow it down.

## Cube leaks exactly 6 bytes per frame, and that is measurable

`DUX_REPORT_DEPTH=1` reports the working-stack depth after every frame vector
instead of latching the first failure:

    depth after vector 20: 12
    depth after vector 20: 18
    depth after vector 20: 24

Six a frame, steady. Cube's sort loop runs six passes and calls `@fill` once
each, so this is one byte per `@fill` call - from a routine whose own depth is
**{+0}**.

That is not a contradiction, and reproducing it took fifteen lines: a routine
can be perfectly balanced internally and still leak, because the *caller* pushed
an argument the callee never consumes.

    @on-frame
            LIT $01
            JSR2 @fill      ; @fill is just JMP2r2
    @fill
            JMP2r2
    -> left 2 items

This is the last blind spot in the static tools, and it is the same per-site
versus per-call distinction that made `@fill` look like the culprit before. It is
worse here, because **the graph cuts back edges**: a loop body that leaks one byte
per *iteration* is charged once and reads as balanced. The loop runs six times,
the body leaks a byte, and 6 × 1 is the answer. No amount of path-summing fixes
that, because the trip count is data.

So the standing limits of the tools are now both known and both narrow:
`stacksim.py` cannot see a diamond, `routinegraph.py` cannot see a per-iteration
leak inside a loop, and neither can name a call site. The host's balance check
sees the total, `DUX_REPORT_DEPTH` gives the rate, and the rate is what says
which of the two it is.

## The width table, measured rather than inferred

`tools/probe.sh` runs one instruction through a frame vector so the host's own
balance check reports the result. That found three widths that had been wrong
since the beginning, all of them invisible because a ROM that pairs a load with
a store reads zero under either value:

| form | was | is |
|---|---|---|
| bare `STA` | −1 | **−3** |
| bare `STA2` | −2 | **−4** |
| `ADD2` and friends | −2 | −2 (unchanged) |

The first two because `dux.c` reads `OPC(STA, PO2(a) GET(y), ...)` and `PO2`
always takes two bytes for the address whatever the value's width, so a bare
`STA` eats a short address and a byte. Measured: `LIT2 addr / LIT val / STA`
leaves nothing, and `LIT2 val / LIT2 addr / STA2` leaves nothing.

### And one I got wrong in the act of fixing them

For a moment I also "corrected" `ADD2` to −4, reading the probe's `left 2 items`
as a net of −4 rather than 2 − 4 = **−2**. Every one of the thirty-odd ROMs went
negative at once, which is what gave it away: a width table that condemns every
program on the project is wrong, not thirty programs.

The lesson is the one the table itself records. Eleven widths on this project
were fixed by demanding agreement with known-good programs, and the ones that
survived longest were the ones **no program could disagree about** — a width that
only ever appears beside its own negation cannot be calibrated by agreement. That
is exactly the blind spot a probe is for, and exactly where reading the output
faster than reading the measurement will bite.

## Cube: the last six bytes, and a wrong fix I caught by the numbers

Cube leaked exactly **six bytes a frame**, steady. Six a frame is not a loop that
runs six times by accident: the sort loop runs six passes and calls `@fill` once
each, so it is one byte per `@fill` call - from a routine that reads {+0}.

The site is four instructions:

        LDZ $ZP.bi
        STA $ZP.j
        LDZ $ZP.j        <- pushed, and never consumed
        JSR2 @fill

The face index was stored in `$ZP.j` and then pushed again, as though `@fill`
wanted it as an argument. It does not: `@fill`'s first act is `LDZ $ZP.j`, so it
reads the memory itself. The pushed byte was simply left behind, once per sort
pass. Reproduced in fifteen lines and confirmed by removing the redundant `LDZ`:

    store-then-push, callee takes none:  left 18 items   (3 frames x 6)
    without the redundant push:          left 12 items   (a different bug, see below)

**A fix I made and then reverted.** Reading the arithmetic probe's `left 2 items`
as a net of -4 rather than 2 - 4 = **-2**, I "corrected" `ADD2` to -4. All
thirty-odd ROMs went negative at once, which is what gave it away: a width table
that condemns every program on the project is wrong, not thirty programs.

Then I added a `POP` to `@plot` because `DEO $SCREEN.pixel` measures as consuming
no bytes. That turned cube's leak into 148 items, because `@plot`'s three entry
stores already take all three arguments - the 255 the probe reported was the
*probe* underflowing, not a leak in the routine. Reverted.

Both mistakes have the same shape: a plausible mechanism, applied without asking
whether the numbers move the right way. The rate made both obvious in one run.

## Where both ROMs stand

| | |
|---|---|
| `clock.rom` | balanced at every frame; 103 px predicted from the segment table, matched first run |
| `cube.rom` | balanced at every frame; three faces in crimson, green and yellow as it turns |

Cube's fix changed no pixels: 8490 lit and one colour at two frames, exactly the
recorded prediction, before and after. That is the check that matters here - a
stack repair should be invisible on screen, and if it is not, the wrong thing has
been removed.

## GTH2 and LTH2: measured, and the opposite of what the code suggests

`dux.c` reads `OPC(GTH, POx(a) POx(b), PU1(b > a))` with `a` popped first, which
looks like *second > first*. That is backwards. Measured by drawing a pixel at
x = 100 when the branch is taken and x = 200 when it is not, with the two
operands set to known values:

    LDA2 $0020 / LDA2 $0010 / GTH2 -> taken      (x = 100)
    LDA2 $0010 / LDA2 $0020 / GTH2 -> not taken  (x = 200)
    LDA2 $0020 / LDA2 $0020 / GTH2 -> not taken  (equal is not greater)

So `GTH2` is **first pushed > second pushed**. The stack order is the operand
order, deepest first, which is also how `LDA2 x / LIT2 k / GTH2 / JNZ` reads as
"x > k" - the form every ROM here uses.

This caught a fix of mine from earlier in the same session. Clock's erase loop
should cover the eight rows the digits occupy, and I had "corrected" it to
`LIT2 $0009` on the reading above. That erases **ten** rows - confirmed by the
sprite trace - two of them below every digit. `LIT2 $0007` is right: the flag is
`py > 7`, the loop exits at py = 8, and the trace now shows y = 90..97 exactly.

The pixel count did not move (103 before and after), because the two extra rows
were below the digits and nothing was ever drawn there. **A fix that changes no
output and is still wrong is the dangerous kind**: only the device trace showed
it. Reading the C source is not measuring the machine, and this is the third time
on this project that the two have disagreed - `AND2`'s width, bare `LDA2`, and
now the comparison direction.

## Nine more ROMs, and four predictions of mine that were wrong

Nine of the quarantined programs are real, assemble, and are stack-balanced.
They were blocked for want of a shared prelude beside them: `INCLUDE "common.tal"`
resolves next to the file that asks for it, so nothing in `wip/` could find it
and every one of them failed with `cannot open` before a single instruction was
assembled. Copying the prelude across turned thirteen of them into working ROMs.

| ROM | lit | what it draws |
|---|---|---|
| `band` | 12200 | `0 <= si-sj <= 0x3c`, 61 px on each of 200 rows |
| `wedge4` | 7220 | widens from the corner to 40 wide, then holds |
| `sumband` | 5061 | 42 wide sliding left from x=99, clipped at 0 |
| `step2` | 25760 | `x` from `2*sj..319`, a triangle |
| `rings` | 17572 | concentric rings from min/max per corner |
| `rings3` | 16524 | three rectangles, tested by edge equality |
| `stripes2` | 30720 | 15 rows of 320, every 32 |
| `stripes3` | 35200 | 17 rows of 320, every 32 |
| `vstripes3` | 34000 | the transpose: 170 columns of 200 |

Every count was derived from the source's own predicate before rendering, and six
matched first time. The four that did not are all the same mistake on my part:

**I recorded drawn pixels where the drawing *is* the majority colour.**
`ppmcheck.py` takes the background by majority unless told otherwise, so:

- `rings` fills 73% of the screen and `rings3` 74%: the ring colour *is* the
  most common pixel, and 48384 was the drawn count rather than the lit count.
- `stripes3` and `vstripes3` draw colour 1, which `common.tal`'s palette makes
  `(204,136,255)`, and 35200 of 64000 is a majority.

All four needed `--bg`, which is exactly what the override in `expect.txt` exists
for and which `twotone`, `vstripe` and `dots` already needed. The ROMs were right;
the predictions were not, and the verifier caught it.

## The project now

43 ROMs, all distinct (checked by hashing every frame-4 image), every one
verified against a prediction written down in advance, and every one returning
an empty stack after all eight frames. `cube.rom` and `clock.rom` both balanced,
both verified across frames, cube turning through crimson, green and yellow.

`tools/balcheck.sh` is new and wired into `make verify`: it watches the depth
after *every* frame rather than latching the first failure, so it reports a rate
instead of a total. That is what identified cube's six bytes as one per sort pass
rather than something at reset, and it is now a standing check rather than a
thing that had to be typed by hand.

## Four more ROMs, and a checker that was quietly wrong

`checks` would not assemble because it carried its own copy of `@widen`, a label
the prelude had already defined. Deleting the copy was the whole fix; the ROM
draws four dots and a 16x16 block, and the colour-0 dot is *deliberately*
invisible because the prelude's palette makes colour 0 the same as the
background. **259 pixels, three colours**, predicted from that reading and
matched first time.

`wedgev` had a `%%COUNT%%` placeholder where a row bound should be, which is why
it read as an unknown mnemonic. With it gone the figure is a V whose point walks
left with the row. I predicted 12656 by treating `0xdf - si` as arithmetic that
stays positive; it does not, for `si > 223`, and the render is **12356** -
`x = 0..min(y, 223-y)` on each of 200 rows. The prediction was wrong and the
verifier said so.

`blocks3` was byte-for-byte `blocks`: the second file was written and then
overwritten by the first. `hstripe` was `stripes2` wearing a different mask that
happened to select the same rows - `(sj & 0x0b) <= 5` and `(sj & 0x0f) <= 7`
both pick 104 rows. Two duplicate pictures shipped with matching predictions,
which is exactly the failure a prediction table cannot see: a right number
reached twice.

So `tools/distinct.py` exists now, hashing four frames per ROM and wired into
`make verify`. Both duplicates were replaced with genuinely different figures -
`hstripe` is five rows on of every eight (40000 px), `blocks3` is a brick rather
than a square (5600 px).

### ppmcheck.py's majority rule bit me once more

`stripes2` draws 33280 pixels against a ground of 30720, so the *drawing* is the
most common colour and the checker inverted the two. It reported 30720 lit where
the framebuffer held 33280. The prediction in the table was right all along; the
tool silently mislabelled it. `--bg` exists for exactly this and is now used by
five ROMs.

A counting tool with a wrong answer is worse than no tool, because it is trusted.
The count is now cross-checked against an independent reading of the framebuffer
whenever a total looks like a near miss.

## Three more ROMs, and one I did not ship

`xmark` and `zigzag` both tested their predicates with `EQU2 / JNZ`, which is
inverted: `EQU2` pushes one **on** equality, so `JNZ` skipped the matching case
and neither figure appeared at all. `NEQ2` pushes one on inequality, which is
what a jump-to-skip wants. `xmark` is now a complete X - two diagonals of 200,
**400 pixels**, predicted and matched - and `zigzag` bounces off x = $a0 for
**320**, one pixel a row.

`ramp` pushed one byte to `@plot`, which wants five: x, y and a colour. It drew
nothing. `@plotat` reads the coordinates from the zero page instead, which is
what every other figure here uses, and `ramp` now draws **2304**.

`checkera` stored its colour with `STA $ZP.t` and *then* called `@plotat`, which
pops its own argument and so took whatever sat underneath. The figure is a
288x168 region in two chequer colours - **48384** lit, 24192 each. `ppmcheck.py`
had reported 39808 because its majority rule split that exact tie and called half
the figure background, which is the failure mode `--bg` exists for.

### `hash`: not shipped

I rewrote it three times and it never became a ROM whose picture matched its
description, so it is out rather than in. What I know:

- It assembled, ran, and ended every frame balanced.
- As bytes it drew a solid 256x200 block, because a byte `MUL` wraps at 256 and
  `LDZ` on a short counter reads only its low byte, so the right-hand quarter of
  the screen was unreachable.
- Widened to shorts it scattered properly - **3520** dots, near the 3000 the
  source predicts - but leaked **18 bytes** per frame, and the routine graph
  could not localise it.

The honest summary is that I could not finish it, and a ROM in `roms/` whose
comment describes a picture it does not draw is worse than no ROM. The other
four scratch files from `wip/` are ISA probes (`probe12`, `probe14`, `probeA`,
`probeB`) that draw nothing by design; they are diagnostics, not programs, and
they stay out.

## Four probes of my own that were wrong before the ROM was

Worth recording, because each cost more than the fix it was hunting:

1. `LDA2 $0020` where I meant a literal y. `$0020` is `@ZP.ex`, so the store
   aliased the very scratch the test was reading - and the comparison always
   looked equal.
2. `LDA2 $0020` again as the "x" for a pixel, where `$0020` was again a symbol.
3. A `getenv` inside a switch case in `dux.c`, which is unreachable C.
4. Pushing x, y, colour to `@plot` in the order x, y, colour. `@plot` pops
   colour, then y, then x, so my x landed in the colour slot and every probe
   drew at x = 0 - which is what made me briefly suspect `@widen`, a routine
   `checks.rom` proves works.

Not one of those was a machine fault. Every one was me writing a test that could
not fail, and then believing the answer it gave.

## The ISA probes are now a check, not scratch

The four files in `wip/` called `probe12`, `probe14`, `probeA` and `probeB` are
not ROMs: they draw nothing and print to the console, because what they are for
is answering a question about the machine and being read. All four still run and
all four still report what they were written to establish:

    probe12  1,0,0,1,155,165,1,0,0,1,155,165,
    probe14  0,1,1,0,10,0,1,1,0,10,
    probeA   10,10,8,8,10,10,8,8,
    probeB   10,10,8,8,10,10,8,8,

`probe12` is the one that matters most: **GTH2 tests the first value pushed
against the second**, so with si = 10 and sj = 5 the pair reads (1, 0) and with
si = 5, sj = 10 it reads (0, 1). That is the measurement behind the erase-loop
fix in clock.tal, and it is the third time the C and the machine have disagreed
on this project after `AND2`'s width and bare `LDA2`.

`tools/probe.sh` runs them and compares the output, wired into `make verify`. A
future change that quietly inverts a comparison now fails a build rather than
producing a subtly wrong picture somewhere else.

**wip/ has a README** saying what is in it and, more usefully, the five defects
that blocked each program, because they are the same five every time: the prelude
not being reachable, a colour stored and *then* handed to `@plotat`, `EQU2` where
`NEQ2` was wanted, the comparison direction, and `STA2` used for a one-byte flag.

## What a 60-frame run shows

Every ROM now holds depth 0 after all sixty frames - no slow drift anywhere, which
is what a leak inside a loop would look like - and none of them approaches the
eight-million step limit.

Of the sixteen programs that could plausibly animate, only **`cube`** and
**`sweep`** change between frames 2, 30 and 60. The other fourteen are byte-for-
byte identical at all three, so a two-frame check is sufficient for them and the
frame count in `expect.txt` is not hiding anything.

`cube` at frame 60 is a cube: three faces, the near corner, and the two receding
edges meeting it, correctly perspective-projected, in crimson, green and yellow.
It holds at 120 frames.

## The sprite port cannot draw thirty-two pixels, and @clear asked for thirty-two

`hash.tal` drew three thousand dots where the model said three thousand and the
framebuffer held **1490**. Not 3000-and-a-bit, not a scatter with the wrong
spacing: 1490, which is neither the prediction nor anything near it.

The debugging was long and every step of it was wrong in an instructive way, so it
is worth writing down in order, because the method is the point.

`hash` computes `(7i + 13j) & 0x3f <= 2` per pixel. I proved the arithmetic was
right by reading the drawn pixels' hashes out of the render - all 1490 of them
computed to exactly 0, 1 or 2 under the model, across all 200 rows and all 320
columns. Then, on a hunch, I read out the machine's *own* arithmetic instead of
inferring it, by replacing the plot with a `@show` and printing hash and flag for
all 64 columns of 8 rows as text. Every hash and every flag was exactly as
modelled: `0,0, 7,1, 14,1, 21,1, ...`. So `MUL2`, `ADD2`, `AND2`, both counters
and `GTH2` were all correct, and none of the nine probes I had written to test
them found anything, because each of them was wrong in its own way.

The pixels that were missing were missing *as pixels*: the device received exactly
the writes the model predicted, at exactly the right coordinates, and a third of
them were not in the render afterwards. `DUX_TRACE_PIXEL=1` is what showed the
sequence of port activity, and it read:

    sprite  x2211      <- @clear
    pixel   x29        <- @draw
    sprite  x2211      <- @clear again, after the drawing
    pixel   x3

**`@clear` runs a second time after `@draw` has finished.** The host calls the
frame vector once, and `@draw` clears and then draws - so the second clear belongs
to the *next* `@draw`. Except there is no next `@draw`, because `hash.tal` never
initialised its row counter: `$24` was whatever the previous frame left in it, so
after the first frame the row loop started at row 9, ran one row and stopped. One
frame's worth of work, and a fresh `@clear` in front of it.

That is the small bug. The large one is in the prelude, and it was in every ROM
ever shipped.

`@clear` erases by stamping a flat sprite of colour 0. It stepped x by **32**
pixels per stamp. But bits 5 and 6 of the sprite port give the length *in bytes,
plus one*, so the longest run the port can be asked for is four bytes - and at
four pixels to a byte that is **sixteen** pixels. The stamps covered the left half
of every 32-column block and the right half of the screen was never erased.

**Thirty-two pixels is not drawable in one sprite call.** The width is
`(val >> 5 & 3) + 1` bytes, capped at 4.

It hid for as long as it did because every still picture redraws the same pixels
over the stale ones, so a two-frame comparison cannot see it: frame 2 contains
frame 1's survivors *underneath* the same pixels. Only `cube` and `sweep` animate,
and `cube` covers the whole screen so completely that its own faces buried the
leftovers - 336 pixels of the previous frame, at frame 4, entirely inside the
cube's own silhouette. Fixing the step to 16 changed `cube` by those 336 pixels
and nothing else.

Measured cost of the fix: `@clear` goes from 2211 sprite calls to 4422, which is
about 100k steps against an 8M budget. Nothing is near the limit.

## What this says about the rest of the set

`make verify` still passes for all 51 ROMs, and that is the correct outcome
rather than a lucky one: a still picture's pixel count cannot change when the
pixels underneath it stop being stale. So the fix is invisible to the whole
existing test suite, which is the most useful thing to know about it - a check
that passes is only as strong as the thing it can distinguish, and
"frame 2 looks like frame 2" cannot distinguish a correct erase from a partial
one.

The check that does distinguish it is the pixel-port trace, and the one that would
have caught it without any tracing is a two-frame comparison of a ROM that draws
*less* than the whole screen and *moves*. `sweep` does move, and it never showed
it either, because a growing bar overwrites its own tail. Nothing in the set
caught it; the trace did, and the trace exists because the arithmetic had already
been proved correct twice.

## The stack checker existed and was switched off, and then it was broken twice

`circles.tal` cost most of a day. It drew solid slabs instead of three rings, and
it leaked. Four separate defects, and the last two only showed up once the first
two were fixed, which is the usual way round.

**`&yn-dec` had its guard the wrong way round.** The loop walks `y` down until a
column fits the circle, and stops at zero:

    LDA2 $2e
    LIT2 $0000
    GTH2                    ; a column left to give?
    JNZ &yn-step
    JMP &yn-done            ; the floor is zero, so stop here

written first as `JNZ &yn-done`, which stops whenever there *is* room to give.
`y` never moved, so every column of the circle was a full-height column.

**The run counter walked the wrong way.** `k` starts at the next column's top and
has to climb to this one's; it was decremented, so it went 98, 97, ..., 0, 65535
and stopped when it finally exceeded the top. Two hundred and fifty-eight
thousand pixel writes for a figure with three thousand in it.

**Three of the eight points a column stands for never loaded their x.** Each block
computes a coordinate pair into `$3a` and `$3c` and calls `@plot`, which wants
both. Three of the blocks pushed only `$3c`, so `@plot` took its x coordinate
from below the bottom of the stack - which is how the depth went to 232 and 80
and 4 rather than to zero. **`ADD2` with two bytes on the stack** is the same
fault: it pops four, and the assembler does not notice, because it models an
opcode by its net width.

None of those four was found by running the ROM. All four were found by
`duxasm -X`, which had been in the tree the whole time and was not being used -
and when I finally turned it on it caught none of them, because it was itself
broken in three separate ways.

**It could not see past `@clear`.** Every program calls `@clear` first, and
`@clear` is a loop. The walk followed a backward jump, walked the loop body
again, and did so until the four-million-step budget ran out - then returned
"no problems" in the same thirty milliseconds either way. A loop now records the
depth it was walked at and is not walked again at the same one; a backward jump
onto ground already covered carries on down the path instead.

**Its shadow stack belonged to the walk, not to the path.** One `Byte sh[]` lived
in the shared state, so walking a callee zeroed its caller's shadow, and every
branch target after the call read as unknown. A checker that stops following and
says nothing is worse than one that stops following and complains.

**It treated a routine called at two depths as an error.** That is not an error:
cube calls its `@widen` with one byte from nine places and three from a tenth,
and all ten are correct. What says a call site is wrong is the pop *inside* the
routine that runs off the bottom. So a routine is walked at each depth it is
reached at, and reports for itself. That change turned the four circle bugs into
four ordinary underflow reports, which is the shape you want.

**And it needed to know a tail call from a loop.** `@plotat` ends with
`JMP2 @plot`, which points *backwards* and is indistinguishable from a loop's
back edge by direction alone - and mistaking it for one walked `@clear` with five
bytes on the stack, which is where cube's false positive came from. An
unconditional jump is now walked as a call whose depth is carried straight out,
and the fall-through for a loop's back edge is bounded by where the walk
started: a jump below that has left the routine.

**What all of that bought.** `-X` is in the build now, for `make roms` and again
in `make verify`, and every ROM passes it. And it found a real bug in the ISA
self-test:

    @spare-bad
        LDZ $ZP.sp0
        LIT $00
        EQU
        JNZ @spare-report      ; the flag is all that is left: EQU ate both
        LDZ $ZP.sp1
    @spare-report
        LIT $00
        JSR2 @check             ; one byte where @check wants two

`EQU` consumes both of its operands, so the branch to `@spare-report` arrived
with an empty stack and pushed a lone zero where a value and an expectation
belonged. `@check` read its "got" from below the bottom of the stack. **Nothing
noticed for the whole life of the file, because that path only runs when a check
has already failed - the one path a self-test never walks.** The comparison now
happens before the pair is built.

**And it earned its keep on cube too, in the end.** `@clear`'s sixteen-pixel step
was found by the pixel trace. A stray `LIT2 $0000` in the sort - which I removed,
watched the cube start leaking twelve bytes a frame, and put back - is the sort
routine's way of balancing, and the reason is still not something I can account
for from the source. cube's one remaining report is leftovers at a break on a
path where the sort loop runs twice instead of six times, so `-X` calls that a
warning and not a problem; the host's own balance check, running the real thing
for sixty frames, is what settles it.

**The standing lesson is the one from the start, wearing a different hat.** Every
conclusion here came from reasoning about code I had not run, and every one of
them was wrong. The measurement that settles a question is always the cheapest
one to run and always the last one I reach for.

## Four more measured facts, and a way of writing a figure that hangs on three of them

`bounce` and `rays` were built after `-X` was in the build, so their stack faults
were found at assembly time rather than after an afternoon. What they cost was a
set of ordinary mistakes, all of them mine, all of them about which way a
comparison goes. They are worth writing down because each one produced a program
that ran, balanced, and drew the right number of pixels while being wrong.

**`GTH2` and `LTH2` set their flag when the comparison is *true*, and `JNZ` takes
the branch when that flag is set.** Measured, because I had it backwards four
times: `LIT2 5 / LIT2 40 / LTH2 / JNZ somewhere` jumps when 5 < 40. So a test
written as "is it against the wall? / jump if so" has to jump the *other* way,
because the flag is set exactly when the answer is yes. Written the way it feels
like it should read:

    LDA2 $22
    LIT2 $0005
    GTH2                    ; clear of the left wall?
    JNZ @turn-x-far         ; the branch is the normal case
    LIT2 $0005
    STA2 $22                ; the fall-through is the wall

**`SUB2` is first minus second**, so negating a value needs the zero pushed
first. `LDA2 v / LIT2 0 / SUB2` is `v - 0`, which negates nothing at all and is
very happy about it. This is the fourth time this has bitten, and the other three
were the same mistake.

**The screen has four colours and one of them is the background.** The colour
byte is two bits a channel, so a fourth foreground colour masks down to zero and
the pixel disappears into the background instead of appearing as a new colour.
`rays` was written with four colours and rendered with three, short by exactly
the fourth colour's share. There are three usable foreground colours, not four.

**The pixel port drops any write outside the screen, on all four sides, negatives
included.** (319,199) is the last coordinate it takes; (320,199), (319,200),
(400,100) and (-1,100) are all refused. Nothing is wrapped, so a routine that
draws along a direction needs no bounds test at all: walk far enough and the
edge takes care of itself.

**And the one worth remembering: a correct pixel count is not evidence that a
figure works.** The block in `bounce` sat against the left-hand wall for three
hundred frames, drawn its twenty-five pixels every time, cleared the screen every
time, left the stack empty every time, and passed every check the build runs. It
had been frozen by one inverted comparison. The count was right because the count
was never the thing that had broken. What caught it was printing the position
after six hundred frames and seeing that it had not moved - which is the cheapest
possible check and the one that had to be thought of deliberately.

## What `-X` cannot see, and what it cannot see either because the program is right

`weave` is three tests and a counter, and it took longer than `circles` did. Two
of its faults were invisible to the checker, and both are worth stating as what
they are rather than as what went wrong.

**`-X` counts bytes, not arguments.** `JSR2 @plot` with five bytes on the stack
is five bytes on the stack whichever order they are in. `weave` pushed the colour
first and the coordinates second, and `@plot` takes its three values off the top
in the order colour, y, x - so it used the high byte of y as the colour and the
coordinates came out of the colour. Every depth was right. The program drew
fifteen million pixels before the frame ran out of steps, because the coordinate
it passed was not a coordinate.

That is the third time argument order has cost an afternoon, and it is the one
class of mistake a stack checker cannot make. What it can do is catch the depth
mistake, which is the more common one, and it now does that through `@clear` and
everywhere else.

**A missing increment is invisible to everything.** `@row-step` advanced the
counter and the row test compared the row, and the three instructions in between
that moved the row simply were not there. The stack was even throughout, `-X`
was silent, and the figure drew its top row over and over until the frame ran
out of steps. Nothing in a static pass can see it: every instruction is
well-formed and every stack is balanced. The loop is bounded in the code and
unbounded in the run, and the only thing that distinguishes them is running it.

**And `EQU` with `JNZ` skips the case it matched.** The checker has no opinion
about this, and neither does any depth analysis: the program is balanced either
way. All three of `weave`'s line tests were written with the branch on equality
and the fall-through into the drawing, so every test skipped the line it was
looking for, and eight steps out of nine got drawn instead of three, in whatever
colour was left over. The pixel count was 7111 - exactly the count of *one* of
the three lines - and it took looking at the picture to see that the other two
lines were missing and something had been drawn where nothing should be.

**The rule that would have saved all of them.** `GTH2`, `LTH2`, `EQU2` and their
byte forms set their flag when the comparison is *true*, and `JNZ` takes the
branch when that flag is set. So a test written "if this is the case I want to
skip, jump over it" has its two halves the wrong way round, and the program does
the thing you meant to avoid. That happened seven times across `circles`,
`bounce`, `rays`, `weave` and `spiral`, always on a loop bound, always with a
figure that still drew something.

So: **put the branch on the case you want to act on.** If the growth belongs on
the second leg of a pair, then

    LDZ $7a
    LIT $02
    EQU                     ; sets the flag when the two match
    JNZ @leg-grows          ; the branch is the growth
    JMP @turn-done          ; falling through is the skip

rather than the same test with the branch pointing at the skip and the growth
written underneath. The second form is not wrong-looking. It is exactly as long,
it reads the way the sentence reads, and it does the opposite.

What all seven had in common is that nothing could see them. The stack was even,
the ROM assembled, the checker was silent, and in five cases out of seven the
pixel count was *right* - just at the wrong places, or with the wrong figure in
place of the intended one. There is no analysis that finds this, because every
instruction is doing what the program says and the program says the wrong thing.
The only thing that finds it is looking at the picture, or printing something
that moves.

## Black and white, and four ROMs

The set was cut down to four programs - `paint`, `clock`, `bounce`, `cube` - and
the palette changed from colour to greyscale. Both are reversible: the other
fifty-six sources are in `wip/shipped/` and `make roms` will rebuild any of them
by moving the `.tal` back into `asm/`.

**The palette is now one ramp, in `0x0RGB` with four bits a channel:**

    c0  $0000  black
    c1  $0fff  white
    c2  $0999  grey 153
    c3  $0555  grey  85

Which is what `clock` and `bounce` want in full, `paint` wants for its first two
slots (it starts on slot 1, so it comes up with a white brush on a black ground
and keys 1 to 4 give black, white and two greys), and `cube` wants for its three
faces. The cube's own header used to argue against three steps of one colour -
"three shades of blue look like one face" - and that argument is right in
general. It is wrong here, because the faces are also told apart by where they
are, and white, 153 and 85 on black are far enough apart that all six faces read
at a glance. The claim was tested rather than argued, which is the lesson.

`host.c`'s default palette changed with it. It only shows up in a program that
writes no palette of its own, and all four programs write their own, so this is
cosmetic - but a blank program should look blank, and on this machine blank is
black now rather than the near-black navy it was.

**`tools/expect.txt` is positional and I broke it.** `verify.sh` reads the
`DUX_TIME=` assignment from **field 5**:

    env_assign=$(awk -v r="$rom" '$1==r{print $5}' $EXPECT)

So the line has to be `name  lit  colours  -  DUX_TIME=... note`, with the `-`
for "no background override" keeping the note in field 5. Removing the override
because the background is black now, and closing the gap, silently stopped
pinning the clock: it read the real wall clock, drew whatever the time happened
to be, and failed on 75 pixels against a prediction of 103. The digits were
fine. The table was not.

**And I deleted the four ROMs I was meant to keep.** The prune was a shell loop:

    for r in roms/*.rom; do
      b=$(basename "$r" .rom)
      case "$b" in $KEEP|test) continue;; esac
      rm -f "$r"
    done

`$KEEP` holds `paint clock bounce cube` with spaces in it, and a `case` pattern
expands without word splitting - so the pattern was the literal string
`paint clock bounce cube`, which matches no single basename, and every ROM fell
through to the `rm`. All sixty-one went. `test.rom` survived only because
`test` was the second alternative in the pattern and so still matched.

What saved it was that the `.tal` sources had been *moved* rather than deleted,
so `make roms` rebuilt the four in a second. Had I run `rm` on the sources too,
the afternoon would have been longer. A list built by interpolation and used as
a pattern needs one entry per alternative, written out, or an array and a loop
over it - not a single string.

---

## The DVD logo: reflection, not clamping, or the colour never changes

`bounce.rom` became the DVD logo: the wordmark on a disc, 51 x 38, moving ten
across and six down, and changing colour when it reaches a corner and not
otherwise - because a logo that changed colour every time it touched anything
would not be the logo.

Both halves are bitmaps in ROM rather than code, at three times size: seventeen
column masks for a 17 x 7 wordmark and fifteen half-widths for a 51-wide disc,
1938 pixels. 1011 of them lit, on every frame, which is the count that says it
never clips.

The corner rule needed care. The obvious implementation clamps: move, and if the
logo is past a wall, put it back on the wall and turn it round. **Clamping makes
corners impossible.** Each bounce throws away the overshoot, which is enough to
stop x and y ever reaching a wall on the same frame - over four thousand frames
with these steps, no corner at all, so the logo bounced about forever in one
colour. Reflecting instead - past the wall by three, put it three short of it -
loses nothing, and then the corners arrive on frame 293 and every twenty-seven
frames after that, exactly, which a Python search over the step pairs confirmed
before the assembly was written.

Reflecting takes two subtractions rather than one, because `269 - (x - 269)`
needs the overshoot kept somewhere: two `SUB2`s in a row would want four bytes
for the second one.

And the wall test is not "greater than 269". It is "past 32767", which for a step
of ten can only mean off the left edge, since the arithmetic wraps and a negative
sum arrives as 65526.

`$0115` for 269 is 277. My comment said 269 and the hex was wrong, and the logo
went eight pixels off the right edge on some frames - the count caught it at 969
instead of 1011, which is the whole reason the count is written down first.

## The hit flags were set but never cleared

Both wall flags are cleared at the top of each frame. Without that, once the logo
has touched every wall both flags stay set and the colour changes on *every*
frame from then on - a logo that strobes rather than one that changes at corners.
The pixel count and the colour split are identical either way; only watching it
over time shows it.

## The disc came out as its own outline, then as one pixel a row

Two bugs in one routine, both invisible to `-X`, both invisible in the pixel
count's shape.

`@block3` counted its three columns and never moved along them, so all nine
pixels of every 3x3 block landed on the same one and the wordmark came out as a
dotted line - 153 pixels instead of 1011. A tested counter is not a moving one;
this is the fourth time.

`@hside` then drew the disc's rows one pixel wide. It counts upwards from `$30`
to `$2c`, so `$30` has to be the *left* end and `$2c` the right - the opposite of
what the names suggest - and the version that copied `$2c` into `$30` on entry
clobbered the right end the caller had just set. Getting it the wrong way round
does not fail; it draws exactly one pixel a row, which is the *outline* of the
disc rather than the disc, and looks more like a deliberate design than a bug
does.

## A short's high byte is at the lower address, and a byte read finds it

`STA2 $32` with the value 76 writes `$00` at `$32` and `$4c` at `$33`. `LDZ $32`
therefore returns **zero**, not 76.

Every radius in the clock is under 256, so they are bytes now. This one produced
the most confusing failure of the lot: `@offset` returned 0 for every input, so
every hand was zero-length, so the face was empty except its middle - and the
stack was clean, the depths were 0, and a wrong-depth probe said the arithmetic
was fine, because it had stored the radius as a short and read it as a byte too
and got a *consistent* wrong answer rather than an inconsistent one.

**A probe that shares the bug under test proves nothing.** The one that settled it
plotted `byte * r` and `byte * r / 128 - r` for four inputs and compared against
hand arithmetic - different code, different path.

## `DIV2` is first-then-second, and truncates

Measured, not read off: `LIT2 $03e8 / LIT2 $0080 / DIV2` is **7**, and the other
way round is 0. So it divides the first thing pushed by the second and throws
the remainder away - 1000/128 = 7.81 truncating to 7.

## Operands come off the stack, not out of the code

`LDZ $32` assembles to `80 32 10` - `LIT $32` followed by a bare `LDZ`, and `LDZ`
takes its index from the stack (`PO1` pops; it does not read the next byte). So
`LDZ $nn` reads zero-page `$nn` and `LIT $nn` pushes `nn`. Which means `LDZ $01`
pushes *the contents of zero-page address 1*, which is zero - not the number 1.
Half the debugging of this ROM was a probe of my own making that read a colour of
zero and drew nothing, and I read that as "the ROM draws nothing".

Similarly `LDA2 $00a0` loads the short stored at `$00a0`; `LIT2 $00a0` is the
literal 160. In a probe these are silently different numbers, and in the ROM they
are silently different addresses.

## A table of pairs, indexed by entry, reads the wrong byte

The direction tables are two bytes to the entry - an x byte and a y byte - and
the index counts entries, so it has to be **doubled** before it addresses
anything.

Reading `base + index` is not an out-of-range read and not a crash. It reads a
perfectly good byte of the table, just the wrong one, so every hand comes out one
entry behind: the face still looks like a clock, it is just showing a different
time. There is no `DUP2`, so the doubling is `base + index + index` with the
index loaded twice.

## A radius the caller still needs must not be the cell @hand writes

`@hand` needs the inner and the outer radius in turn. Storing the outer over the
cell the caller had put the inner in meant the **second** mark of twelve was
drawn with both ends on the same radius - one pixel long. Eleven of the twelve
marks came out as single dots, and the rim looked like a deliberate dotted circle
rather than twelve dashes.

`@offset` has its own radius cell (`$68`) for exactly this reason. The general
shape: a routine that reads two of the caller's values should not write to either.

## The error term has to lose the short axis on *every* step

Bresenham on an unsigned machine. The textbook form keeps `dy` negative and
starts the error at `dx + dy`, which is already negative on the first step; from
there `e2 < dy` and `e2 > dx` both answer wrongly, neither end of the line ever
moves, and it runs until the step budget runs out - a hundred and seventy-eight
million pixel writes on one vertical line, found by asking for a picture and
getting a timeout.

The repair: start the error at half the long axis and never let it go negative.
That needs the comparison *before* the subtraction, which then has a second trap -
**`JNZ` goes when the comparison is true**, so it cannot be used to skip its own
result. Branching the obvious way round skips the step exactly when the step is
wanted, and every line comes out as a staircase running off at forty-five degrees.
The step has to be the taken branch and the fall-through the skip.

And then the easy one to leave out: when the test says *do not* step the short
axis, the error must **still** lose the short axis. Leave it alone there and it
never changes again, so the short axis moves once and stops, and every diagonal
falls a few pixels short of its far end. On the clock that was four of the twelve
marks, each one pixel short of where it belonged.

Checking a line walk against a signed reference does not help, because the two
differ at the midpoint tie-break and both answers are correct lines. What does
help is testing the properties the drawing needs - and over 6400 lines, every one
of them reaches its far end, has `max(dx,dy)+1` points, never jumps more than one
in either coordinate, and never visits a pixel twice:

    for every dx, dy in 0..39 and every sign pair:
        endpoints correct, count correct, steps adjacent, no repeats

## Swapping the ends of a line draws its transpose

There are two loops in `@line` rather than one because the loop picks its own
axis. Swapping the endpoints does not make x the long axis - it only reverses the
line, so a vertical line still has both its x values equal - and exchanging the
roles of x and y draws the transpose of the line wanted. A vertical hand came out
as its own diagonal. A transposed clock hand points somewhere else entirely.

## Twelve marks, eleven

`LIT2 $0036` is **54**, not the 60 the comment said, and the loop stopped past 54,
so the mark at index 55 was never drawn. Decimal sixty is `$3c`; `$36` is fifty-four.

## The hour hand: six steps to the hour, one per ten minutes

`@d5` is five degrees to an entry. An hour is thirty degrees, so six steps. A
minute is half a degree, so **ten minutes to a step** - the count is `m / 10`, and
at most `11 * 6 + 5 = 71`, the last entry, so the index cannot run off the end.

Two ways to be wrong here, and both look right:

  - **dividing by twelve**, because a twelfth of an hour is five degrees. The
    table is already a five-degree table, so twelve makes the hand move in
    five-minute jumps and sit four degrees short at twenty to.
  - **multiplying the twelfth by five**, which is the same mistake wearing a
    different hat. It points the hand at five times the time - 3:14 reads as 3:50
    - and from ten past the hour it walks off the end of `@d5` into `@d6`'s first
    bytes, which is not even a wrong answer, just a different wrong one.

Twenty-six of seventy-seven sampled times disagreed with the model before this
was fixed. All twenty-six were the hour hand, all by a few pixels, all in one
direction - which is what a systematic index error looks like and what a
tie-break does not.

The clock counts to twenty-three, so `h mod 12` is **two** subtractions and not
one: subtract once and the thirteenth hour is left at twelve, and twelve times
six is seventy-two, one past the end of a seventy-two-entry table.

## Predicting the picture before rendering it

`tools/clockmodel.py` draws the whole face in Python - the same tables, the same
integer division, the same line walk - and prints the pixel count. It is
prediction, not a description: written from the assembly, so when the two disagree
the ROM is wrong and the disagreement is the finding.

That is how the eleven-of-twelve marks, the four short diagonals and the
five-times hour hand were all caught without a picture. Over 101 times - every
hour on the hour, quarter-hours, minute boundaries and forty at random - the
model and the ROM agree to the pixel and to the colour of every pixel.

    python3 tools/clockmodel.py 10 9 36
    python3 tools/clockmodel.py 10 9 36 --ascii

The count depends on the time, because the hands cross and one hand's grey over
another is a different colour. So `analog.rom` is verified with `DUX_TIME` pinned,
to 10:09:36 - where all three hands are clear of each other and of the marks, which
makes the colours separable too. 276 lit: 48 white, 168 grey 153, 60 grey 85.

`DUX_TIME` is the host's **local** time, so it is the epoch plus the local offset:
1791102840 is 08:34:00 UTC and reads as 12:34:00 on this machine.

## And one thing lost

`bounce.tal` was the bouncing square - twenty-five pixels of white, colour 1, and
its prediction said `25 1` for a long time - and it was rewritten in place as the
DVD logo on request, without the old source being copied into `wip/shipped/`
first. The other fifty-six retired programs were moved rather than deleted, and
that is what made the prune recoverable; this one was overwritten instead, and the
`mv` was the step I skipped.

It cost a drawing, not a lesson, but the two are the same lesson: a program being
*replaced* deserves the same `mv` as a program being *pruned*.

---

## Pong: the operand order, measured because I had guessed it wrong twice

`GTH2` and `LTH2` take the **first** value pushed as the left-hand side, and
`ADD2`/`SUB2` net -2 over the two. Measured, not read off the macros, which had
been read and got backwards:

    LIT2 $0005 / LIT2 $000a / GTH2      -> 0     so it is (5 > 10), first-pushed
    LIT2 $000a / LIT2 $0005 / GTH2      -> 1
    LIT2 $0005 / LIT2 $000a / SUB2      -> 65531 so it is 5 - 10, first minus second

Every arithmetic and comparison in `pong.tal` is now written one comparison per
line with its own scratch, because **three pushes before an `ADD2` compare the
wrong pair and nothing complains.** `LDA2 x / LIT2 k / ADD2` sums the two; a third
push in between does not join the sum, it ends up as the other operand of the
comparison. In the player's paddle test that read as *bx + 18 + 3 against 18*,
which is true for every ball on the screen, so the left paddle never returned the
ball and the score ran away 5-0. Three such sites; a scan for the shape now runs
after every edit:

    for each ADD2, if the three lines before it are two or more pushes, complain

## `LDZ $02` is the contents of zero-page address 2

`LDZ $nn` reads address `nn`; `LIT $nn` pushes the number. Writing `LDZ $02`
because the colour is 2 reads a byte of zero page that is zero, so the centre line
was drawn in colour 0 - present in every trace, invisible on the screen, and
`ppmcheck` reporting a smaller count than the model with no other explanation.

The same slip killed the ball: `LDZ $01` is zero page address 1.

## A short at `$4e` owns `$4f`, and the paddle's x wrote over the colour

The paddle colour lived at `$4f`, one past the short holding the paddle's x.
`STA2 $4e` writes two bytes, so the x's **low** half landed in the colour cell:
18 there is colour 2 once the sprite port masks to two bits, and the player's
paddle came out the same grey as the score. The frame still looked like a game of
something, and the colour split said two colours where there are three.

Two neighbouring even cells, a short each, are not two free cells.

## The mouse pair is two `DEI`s and the halves go in the other order

`DEI $95` is the low byte of the mouse y and `DEI $96` the high. A short keeps its
**high** byte at the lower address, so the low half has to go one along:

    DEI $MOUSE.yhi
    STA $46                ; high at the base ...
    DEI $MOUSE.ylo
    STA $47                ; ... and low one along. The other way round reads the
                           ; mouse as its value times 256.

The mouse starts at zero, so this is invisible until something moves it. The first
eleven frames matched the model exactly with the pointer stuck at zero, which is
how it survived as long as it did - and then the paddle appeared to be pinned to
one end of the screen and swinging the wrong way, because 90 was arriving as
23040 and the clamp sent it to the bottom.

**A bug that only shows when the input moves is not a bug the static checks can
find.** The fix that found it: run the ROM with a mouse path and compare against
the model driven by the same path. A parked mouse matched; a moving one did not.

## duxemu's `-m` advances the path before it reads it

`dux -m 160,90;160,90;...` moves the pointer one point per frame, and the index is
incremented *before* the point is read, so the first `host_frame` - game frame 2 -
already sees `path[1]`. Counting from 1, game frame *k* reads `path[k-1]`. Getting
it one point out puts the paddle thirteen pixels out for the whole sweep, and
thirteen is half a paddle, so it reads as a game that is merely stiff rather than
one that is wrong.

Also worth writing down: **`-n N` runs `@draw` N+1 times** - once from the reset
vector, then N frames - so `-n 0` renders nothing at all, and the frame
`verify.sh` checks with `-n 2` is game frame 3. The count happens to be the same
either way here because the ball is in open space at that point; it stops being
the same the moment the ball crosses a line.

## `EQU2` pushes one *on* equality, so `EQU2 / JNZ` jumps out of the match

The ball's English - where it leaves a paddle, decided by *where* on the paddle it
landed - has three cases: above the middle, on it, below it. Written with `EQU2`
and `JNZ` to catch the dead centre, it jumped **away** from the dead-centre case
and gave it the "above" angle, so a ball that hit the exact middle of the paddle
came off at two pixels a frame instead of carrying on straight. The same trap as
`EQU2 + JNZ` skipping a case in an `EQU2`-dispatched table, in a place where the
natural thing to write is exactly the wrong way round.

## A mask is four pixels at two bits, from the top

`$ff` is four across, `$f0` is two, `$c0` is one. `asm/clock.tal` uses `$ff, $c0`
for its horizontal segments, which is **five** across, and `$c0` for its verticals,
which is **one** pixel thick. Both are right there, at clock.tal's size, and wrong
here: mine uses `$ff, $ff` and `$f0`, and the difference is sixteen pixels a frame -
two columns of a digit times four rows, twice over - which is exactly what the
prediction said the ROM was short by.

A sprite byte also *is* the length-2 case twice over: `val = $b2` is flat, two bytes,
two rows, colour 2, from `0x80 | 0x20 | 0x10 | colour`.

## The dash loop that counted once per pass down the screen

Eight on, eight off, and the off-counter advanced once per sweep of all two hundred
rows instead of once per row. The row counter therefore sat still through eight
plots and the centre line came out as two hundred solid pixels. With the counter
advanced per row and the row once per sixteen, it is thirteen dashes of eight.

## A clamp that tests the bound and then returns anyway

The computer's paddle, on the way off the bottom of the screen:

    LDA2 $2c / LIT2 $00ae / GTH2 / JNZ @ai-clip
    @ai-clip: LDA2 $2c / LIT2 $8000 / GTH2 / JNZ @ai-raise
              JMP2r2

It noticed, checked for the negative case, and returned without ever writing the
bound back. So the paddle sat one row off the screen and stayed there.

## Positive means "above zero *and* below 32768"

Used in three places now: the AI's "is the ball coming this way", the paddle
test's "is it travelling into me", and `@ai`'s dead zone. A step of -3 is 65533 as
a short, so one comparison against zero says **yes** for every ball travelling the
other way - the AI chased the ball away from itself and drifted off the screen.

## Predicting a game, frame by frame, rather than one number

`tools/pongmodel.py` implements the rules - the same rules, written twice - and
prints any frame's picture. It found nine defects a single count never could have,
each of which had passed assembly, the stack checker, a balanced stack and a
matching prediction on the frames where nothing overlapped.

And it is the reason to expect the *count* not to be constant: the ball is nine
pixels and a paddle seventy-eight, so crossing the centre line or a paddle changes
it. `python3 tools/pongmodel.py --frames 400` prints the distinct counts the frame
is actually going to take - thirteen of them over four hundred frames, all of them
a dash being hidden or a paddle being overlapped.

The sweep that mattered: **291 frames across seven mouse paths** - none at all,
pinned top, pinned bottom, parked mid-screen, two sweeps, and a path that jumps
about - plus 299 frames of a mouse tracking the ball, which covers rallies,
paddle returns in both directions and a scored point. Pixel-for-pixel and
colour-for-colour on every frame.

`duxsdl` cannot be given a synthetic pointer - its `-m` is *mute* - so the
cross-check of the two front ends runs at mouse y zero, where they agree byte for
byte.

## The digits were too fine to read, and 2 and 5 were the worst of them

`roms/clock.rom` draws six seven-segment digits. It did so with

    @hmask   DB $ff, $c0, $00, $00      ; five across, one row tall
    @vmask   DB $c0, $00, $00, $00      ; one pixel across

so the columns were **one** pixel wide against bars that were one row tall and
five long. Nothing in the picture was thick enough to read at this size, and 2
and 5 suffer most because they are the two digits whose shape is mostly column -
1 is only a column and reads fine, 8 has every segment and reads fine, but 2 and 5
lean on their verticals and their bars being distinguishable, and at one pixel
there is nothing to distinguish.

Both masks are now `$ff, $ff` and `$f0`, and the bar's sprite is `$33` rather than
`$23`. The `$33` was the second half of the problem and was not visible until the
masks were already widened: `$23` is a two-byte sprite with flip-y **clear**, so it
lays down **one row**, while `$33` has flip-y set and lays down two. A digit whose
bars were one row and whose columns were two pixels wide is twice as heavy on its
sides as on its bars, which is as wrong as the reverse.

The digit is now eight by eight, bars eight wide and two rows, columns two wide
and four rows - which is exactly the shape `asm/pong.tal` already used, so the two
clocks now draw the same figures. The counts are 0→48, 1→14, 2→52, 3→52, 4→32,
5→52, 6→54, 7→26, 8→56, 9→54, and `clock.rom` went from 103 lit to **262**.

## The colons were drawn but never erased, so they never blinked

`@erase-all` cleared the six digit boxes. The two colons sit at x=140 and x=180,
which is outside every digit box, so nothing ever cleared them: they were painted
on their first on phase and stayed lit for the rest of the run. The blink was not
a subtle fault - it did not blink at all.

This is invisible to a single-frame check in the only way that matters: on frame 2,
with the colons on, the picture is exactly right. It needs frame 33 or later, after
the phase has turned over, and a comparison against a model that knows what the
frame should be.

Two things came out of the fix:

**Everything a frame draws has to be in the erase list.** The erase was written
around what was obviously drawn and nobody asked what else the frame touched. The
colon now has an `@erase-colon` beside it, and `@erase-all` calls both.

**The erase sprite's height has to match the thing erased.** The first version of
`@dot-off` used `$a0` - flat, two bytes, so eight pixels across - against a dot two
pixels across and **two rows** tall. `$a0` has flip-y clear and so paints one row,
and one row of a two-row dot leaves one row behind: eight pixels, which is exactly
what the sweep then reported, twice the two dots in each of the two colons. `$b0`
is the same sprite with flip-y set and clears both rows.

The sweep that found it: **270 frames**, every digit in each of the six positions,
both colon phases, and the minute and hour boundaries. Pixel-for-pixel.

`tools/digitmodel.py` predicts the whole picture and is what both of these were
found with. Note that `clock.c` reads the device with `localtime()`, not
`gmtime()`, so a model using `gmtime` is four hours out for this machine and
reports a total mismatch that looks like a ROM fault.

## A second declaration of a zero-page name is not a second variable

`asm/clock.tal` declared `@ZP.val = $10`, `@ZP.d0 = $14`, and so on up to `$1c`.
`asm/common.tal` declares the same addresses for its own scratch: `@ZP.c = $10`,
`@ZP.rx = $1a`, `@ZP.i = $12`, `@ZP.fc = $16`, `@ZP.ry = $1c`. A file that includes
`common.tal` gets those *same variables*, so two names at one address are one
variable with two owners.

Nothing was actually broken, and that is the point. `clock.tal` has its own
drawing and never calls `@plot`, `@clear` or `@toflag`, which are the routines that
write `$10` to `$1c`; so today every write is to the cell the writer thinks it is.
The day one of those is called, `@clear` writing its row counter into `$16`/`$17`
would land on `@ZP.d2`/`@ZP.d3` and the last three digits would show `8` forever -
with no assembler warning, no stack-check complaint, and a pixel count that only
moves on some frames. `clock.tal`'s block now starts at `$38`, which neither file
uses; the picture is byte-identical before and after, which is the only reason to
believe a change like that is safe.

## The model's bug looked exactly like the ROM's bug

For most of this the ROM was fine and my *check* was wrong. It reported every one
of sixty frames as having stale pixels, which sent me looking for an erase bug that
was not there, and the reason was one operator:

    (code >> mask) & 1        # wrong: shifts by the mask, not the bit number
    code & mask               # right

That reported `0` as segments a, b, c only, made the model 20 pixels against the
ROM's 36, and turned every digit into a pile of phantom extras. The tell was that
the report was uniform - *every* frame, all six digits, extras but never anything
missing - and a real erase bug shows extras and missing together, because the
digit it disagrees about is wrong in both directions at once.

So when a model says a verified ROM is wrong in a way the assembly does not
suggest, **suspect the model first, and prove the model against something trivially
checkable before believing it.** Here that was printing all ten digit values, one
at a time, and looking at them: the segment codes were right, the bit-to-segment
map was right, and both were being read by a model with the wrong operator.

And printing all ten is worth doing anyway. A seven-segment display has two
separate tables - the code for each value, and which pixel each bit lights - and
reading the code table proves nothing at all about the second one. Ten digits
side by side is the cheapest possible test of both.

## `DB` used to drop its last item, silently

`split_toks` in `asm/duxasm.c` ended `while(n < MAXTOK)` with `MAXTOK 16`, so a
data line with seventeen values assembled to sixteen and said nothing at all.
`asm/bounce.tal`'s wordmark is seventeen columns, so it lost its last one: the
third "D" of the DVD logo came out with no right-hand edge, 27 pixels short of
what the source asks for, and every check that compared a *count* had been
passing against the wrong number.

It is the worst kind of fault for a tool to have, because the symptom is a
picture and not an error. The fix is two parts: `MAXTOK` is 64, and going past it
is now a message rather than a short line.

    duxasm: asm/bounce.tal:485: more than 64 items on one line; put the rest on the next

Scanning every `DB`/`DW`/`BYTE`/`WORD` line in the project for its item count -
splitting on commas *and* whitespace, because `bounce.tal` separates with spaces -
found this was the only line in the whole tree that had actually lost anything.
`analog.tal` has sixteen-item lines, which fitted exactly.

The assembler test suite now pins four cases: eighteen items in `DB`, eighteen in
`DW`, a space-separated `DB`, and a seventy-item line that has to be *refused*.
Expectations are built in the script rather than written out, because when I
wrote them by hand I got two of the four wrong - and a hand-written 36-byte hex
string is a test of my arithmetic wearing a test's clothes.

While checking that, `DW $0001` was found to emit `00 01`: high byte first,
which is right, because a short keeps its high byte at the lower address. My
expectation assumed little-endian and the assembler was not the thing that was
wrong.

## A ROM with no prediction fails the gate, which is the point

`tools/verify.sh` refuses any ROM in `roms/` with no line in `expect.txt`, and
when `blackjack.rom` was sitting in `roms/` half-built it turned `make verify`
red with *"no prediction recorded - nobody checked this one"*. That is the tool
doing exactly its job: the alternative was to write down 460 lit because that is
what came out, which is a transcription dressed as a prediction.

So an unfinished ROM belongs in `wip/`, not in `roms/`.

## The Pong score displayed 8 at ten points (2026-10-05)

Reported as "numbers 1 2 3 4 5 6 7 8 9 still distorted". They were not: all ten
digits render correctly and pixel-exactly, verified by a throwaway ROM that draws
0-9 side by side and checks each 8x8 box against tools' own segment model
(48, 14, 52, 52, 32, 52, 54, 26, 56, 54 lit - model and ROM agree on every
pixel of every digit, not just the count).

The real defect was in the score, not the glyphs. `@segcodes` has ten entries and
`@cm-left`/`@cm-right` incremented a score byte with no cap, so at ten `@digit`
indexed `@segcodes[10]`, read the first byte of `@hseg` ($ff), lit all seven
segments and drew an eight. `@draw-score`'s own comment claimed "the score stops
at nine" and nothing enforced it.

Fixed by counting in units and tens ($5e/$5f units, $2e/$30 tens, carried by
hand - the ROM uses no division instruction) and holding the tens at nine, and by
drawing two digits a side. 461 lit rather than 365.

Two things worth recording:

- **`LIT $b2` is not "flat".** I read bit 4 as clear and diagnosed the digits as
  one row tall. `$b2` has it *set*: `len = ((val>>5)&3)+1`, so $b2 and $32 are the
  same sprite. The two-pass reordering I did first was therefore a no-op, which
  the all-digits harness showed by producing byte-identical output. The harness
  is what made that visible; the reasoning alone would have shipped a "fix" for a
  bug that did not exist.
- **The harness caught a bug the count could not.** Drawing $2e in the units
  position while the carry treated $5e as the units meant a score of 1 would read
  "10". Every pixel count was right throughout - 461 lit either way - because the
  two digits were both legible. Only pre-loading the units digit and reading the
  screen showed it. A count is not a picture.

---

## Measured, while trying to make a gear turn: four things this file had wrong

Every one of these is a silent wrong answer, not a crash, and every one of them
was found by a probe or a trace rather than by reading `dux.c`. `wip/widenprobe.tal`
is the probe; it draws its answers so the picture is the report.

**`@widen` zero-extends. It does not sign-extend.** `@widen` is `STA $7f / LIT
$00 / STA $7e / LDZ2 $7e`: the byte goes in the high half and the low half is
set to zero, so `$ff` widens to 255, not to -1. Probed: widening `$ff` and
plotting it as an x puts a pixel at 255. Two consequences, both of which cost
real time:

  * A table of `DB` offsets cannot hold a negative number usefully. `-7` is
    `$f9`, widens to 249, and the gear is drawn in the wrong place.
  * `@sgn` returns `$ff` for "step left", and `ax + 255` is not `ax - 1`. A
    Bresenham line walks off the far side of the screen, never lands on its
    endpoint, and `@seg` never returns - which presents as the whole ROM
    hanging, with the emulator's own step budget as the only suspect.

`@sgnshort` in `asm/gears.tal` carries a byte sign across to a short by hand.
It is the only place in that ROM that needs to.

**Shorts are big-endian in memory.** `LDZ2 $7e` with the byte at `$7f` and zero
at `$7e` gives 255, so the high half lives at the lower address. Data emitted
low-half-first is not off by a rounding, it is off by a factor of 256: `24`
comes back as 6144.

**`LDA2 $x` is indirect, not direct.** It pushes the address and then reads the
short *at* it, so the core's `LDA` pops a 16-bit address in every mode. To
reach a computed address, push it and then use a bare `LDA2` with no operand -
`asm/duxasm.c` calls this out, and the suffix-free form is the only way. Writing
`LDA2 $p` where you meant "read through the pointer in `$p`" silently copies the
pointer instead of dereferencing it, and the picture is three hubs and nothing
else.

**A label before a large `DB` block resolves to the wrong address when it is
forward-referenced.** `LIT2 @tab11` came out six bytes before the payload, which
is inside the tail of the preceding routine, so the tables were read as
instructions. Moving the data above the code in `asm/gears.tal` fixes it, and
`wip/lbl.tal` is the two-line case that shows a label resolving correctly when it
is *not* a forward reference. Worth knowing before trusting any ROM with a big
table in it: check that the assembler's idea of the label is where the bytes
actually are.
