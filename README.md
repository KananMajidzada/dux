```
  __
 /\ \
 \_\ \  __  __  __  _
 /'_` \/\ \/\ \/\ \/'\
/\ \L\ \ \ \_\ \/>  </
\ \___,_\ \____//\_/\_\
 \/__,_ /\/___/ \//\/_/
```

# dux

A minimalist virtual machine.

---
```
dux/
  README.md           this file: the ISA, the devices, the assembler
  dux.h               public interface, memory map, opcode table
  dux.c               CPU core, no dependencies
  screen.c/.h         the Screen device
  audio.c/.h          the Audio device: synthesis shared by both front ends
  clock.c/.h          the Datetime device
  file.c/.h           the File device: a small persistent store
  host.c/.h           the shared device layer: input, vectors, frame loop
  duxemu.c            headless front end: loader, disassembler, trace, PPM, WAV
  duxsdl.c            SDL2 front end: window, keyboard, mouse, audio
  tests.c             core conformance suite
  tests_devices.c     Screen device suite
  tests_host.c        input queueing and frame loop suite
  tests_asm.sh        assembler regression suite
  asm/duxasm.c        two-pass assembler
  asm/stackcheck.c    static stack balance check, run by the assembler
  asm/test.tal        the self-check: every device, verified from the inside
  Makefile
```

The core and the device layer have no external dependencies. Only `duxsdl`
needs SDL2, and it is an optional target.

## Build

```sh
make          # libdux.a, the headless emulator, and duxasm
make sdl      # the SDL2 front end (skipped if SDL2 is missing)
make test     # all four suites: core, devices, host, assembler
make asan     # the same suites under AddressSanitizer and UBSan
make roms     # assemble every example
make smoke    # run the SDL front end headless and check the capture
```

The core is plain C89 with no dependencies and no dynamic allocation, so it
links into a freestanding target unchanged. Everything platform-specific lives
in `duxemu.c`.

## Run

There is one program, `asm/test.tal`, and it checks the machine. Assemble it
and run it:

```sh
./duxasm -o test.rom asm/test.tal
./dux -n 3 -s /tmp/dux.store test.rom      # headless, no window
./duxsdl test.rom                          # or in a window
```

You should see:

```text
dux self test
37 passed, 0 failed
```

To gate a change on it, match the printed count. A Dux program has no way to set
an exit status, so the count is the result:

```sh
./dux -n 3 asm/test.rom | grep -q ', 0 failed' || echo broken
```

`make test` does that plus the three C suites and the assembler tests, and
`make smoke` runs the same program through the SDL front end with no display and
checks the frame it leaves is not blank.

| Option | Meaning |
|---|---|
| `-c N` | step budget per vector, so a runaway is reported not hung on |
| `-d [N]` | print N instructions, then exit |
| `-f N` | run N frames |
| `-p FILE` | write the framebuffer to FILE as a PPM |
| `-w FILE` | render the Audio device to a WAV file |
| `-s FILE` | load the File device's store from FILE, and save it on exit |
| `-k N` | hold key code N down, for input tests |
| `-t` | trace execution until the next BRK |

`dux` reports two things it cannot fix: a vector that does not reach `BRK`
within its step budget, and a vector that leaves the working stack unbalanced.
Both mean the program is already wrong, and both are worth a line of output
rather than a wrong picture.

## Design decisions (locked)

- 64KB address space, four regions, fixed: zero page, devices, code, video.
- Two 256-byte circular stacks. Both pointers are bytes, so overflow and
  underflow wrap silently. That is the entire policy.
- No interrupts. The host runs a device's vector to `BRK`, then services the
  event. A vector that does not reach `BRK` is reported and cut short.
- 32 base opcodes × 3 mode bits (short, return, keep) = 256 operations, all
  in one byte. 16-bit operands live on the stack, never in the instruction.
- The reset vector is at `0x0200`, so code comes first and data after it.
- Video is 320×200 at 2 bits per pixel: 16,000 bytes of framebuffer, four
  colours, no modes and no scrolling hardware.
- The device layer is one implementation shared by both front ends, so a
  program cannot behave differently depending on which one is running it.

## Memory map

| Range | Use |
|---|---|
| `0x0000–0x00FF` | Zero page. No types: a byte array with conventions on top. |
| `0x0100–0x01FF` | Device I/O, 16 devices × 16 ports. |
| `0x0200–0x7FFF` | Code and data. The reset vector is at `0x0200`. |
| `0x8000–0xFFFF` | Video RAM. The framebuffer is `0x8000–0xBE7F`; the rest is spare. |

## CPU

Fetch-decode-execute over the whole address space. `dux.c` knows nothing about
the host: it calls two function pointers, `dei` and `deo`, and everything
platform-specific lives outside it.

### Stack depth budget

256 bytes per stack, and a routine's arguments are on the working stack while
its return address is on the return one. In practice this leaves room for
roughly a hundred nested calls, which is far more than a 32KB program needs.
The consequence to design around is that **there is nowhere to put a
variable**: the depth *is* the bookkeeping, and every mistake in it is silent.
The assembler checks it (see below).

### Opcode layout

The low five bits are the opcode; the high three are the mode:

```
bit  7   keep   do not consume the operands
bit  6   return operate on the return stack
bit  5   short  16-bit operands
```

Opcode `0x00` is contextual: with no mode bits it is `BRK`, and each other
combination of the mode bits is a different instruction.

### Modes

Every base opcode is written once as a macro that expands to its eight cases.
`ADD2kr` is one byte.

- **short** — operands are two bytes. Without it they are one.
- **return** — pop from and push to the return stack instead of the working
  one. `JMP2r2` is a return; `JSR2` is a call.
- **keep** — capture the operands, then restore the stack pointer, so the
  instruction can read what it is about to consume and leave it there.

### Byte order

Three different orders, all deliberate:

- **On the stack**, low byte on top. `LIT2 $1234` leaves `34` above `12`.
- **In memory**, big-endian. `$1234` at `$0300` is the bytes `12 34`.
- **In a device port pair**, high byte at the base address. `$28` is the high
  half of the cursor x and `$29` the low half.

The third is the opposite of the first, which is why it is easy to get wrong.
`DEO2` and `DEI2` both use it, and the device tests assert it directly. The
practical consequence: **never write a device short with `STA`**, which
addresses flat memory — `STA $2c` writes zero page `$2c`, not port `0x012c`.
Use the bare `DEO2`/`DEI2` forms with the port off the stack, or the two-operand
form `DEO2 $2c`.

### Operand order

Instructions take their operands from the stack, top last:

```
LIT $03
LIT $07
ADD          ; 7 + 3 = 10, because the top of the stack is the second operand
```

`EQU`, `NEQ`, `GTH`, `LTH` and friends **consume** both operands and push one
byte of result. Testing the same value twice means reading it twice, not
`DUP`ing it: a copy left on the stack when a branch is taken is still there at
the `BRK`, which the host rightly complains about.

`JNZ` jumps to an inline target if the stack top is nonzero, and consumes it.
It is a pseudo-op that lowers to `LIT2 target` followed by a short-form jump,
which is the only way the source can put the target *under* the condition.

### Address operands

```
LIT2 $0077          ; push the value
LIT2 $0300
STA2                ; mem[0x0300] = 0x0077
```

`LDA`, `STA`, `STZ`, `LDR` and `STR` take their address from the stack and it
is popped.

### Relative jumps

In byte mode a jump target is a **signed byte offset from PC**, not an address.
An absolute target therefore needs the short form, which is what the assembler
emits for `JMP &label` and `JNZ &label`.

### Opcode 0x00 is contextual

| Byte | Mode | Instruction |
|---|---|---|
| `0x00` | — | `BRK` |
| `0x20` | short | jump to an inline offset if a popped byte is nonzero |
| `0x40` | return | jump to an inline offset |
| `0x60` | short+return | jump to an inline offset, stashing the return address |
| `0x80` | keep | `LIT`, push the next byte |
| `0xa0` | short | `LIT2`, push the next short |
| `0xc0` | keep | `LITR`, push the next byte to the return stack |
| `0xe0` | short | `LIT2R`, push the next short to the return stack |

### Base opcode table

| | byte | short | notes |
|---|---|---|---|
| `0x00` | `BRK` | — | contextual, see above |
| `0x01` | `INC` | `INC2` | +1 |
| `0x02` | `POP` | `POP2` | discard the top |
| `0x03` | `NIP` | `NIP2` | discard the second |
| `0x04` | `SWP` | `SWP2` | swap the top two |
| `0x05` | `ROT` | `ROT2` | rotate the top three |
| `0x06` | `DUP` | `DUP2` | duplicate the top |
| `0x07` | `OVR` | `OVR2` | copy the second to the top |
| `0x08` | `EQU` | | equal |
| `0x09` | `NEQ` | | not equal |
| `0x0a` | `GTH` | `GTH2` | greater than |
| `0x0b` | `LTH` | `LTH2` | less than |
| `0x0c` | `JMP` | `JMP2` | |
| `0x0d` | `JCN` | `JCN2` | conditional |
| `0x0e` | `JSR` | `JSR2` | call |
| `0x0f` | `STH` | `STH2` | stash to the other stack |
| `0x10` | `LDZ` | `LDZ2` | load from zero page, 8-bit index |
| `0x11` | `STZ` | `STZ2` | store to zero page, 8-bit index |
| `0x12` | `LDR` | `LDR2` | load relative to PC |
| `0x13` | `STR` | `STR2` | store relative to PC |
| `0x14` | `LDA` | `LDA2` | load from a 16-bit address |
| `0x15` | `STA` | `STA2` | store to a 16-bit address |
| `0x16` | `DEI` | `DEI2` | read a device port |
| `0x17` | `DEO` | `DEO2` | write a device port |
| `0x18` | `ADD` | `ADD2` | |
| `0x19` | `SUB` | `SUB2` | |
| `0x1a` | `MUL` | `MUL2` | |
| `0x1b` | `DIV` | `DIV2` | divide by zero pushes zero |
| `0x1c` | `AND` | `AND2` | |
| `0x1d` | `ORA` | `ORA2` | |
| `0x1e` | `EOR` | `EOR2` | |
| `0x1f` | `SFT` | `SFT2` | control byte: high nibble left, low nibble right |

`SFT` is the only shift: the control byte says how far, and which way, in two
nibbles. Right happens first.

## Return sequences

`JSR2 &subr` and `JMP2r2` are the call and the return. A routine ends by
popping its return address and jumping to it:

```
@subr
        ...
        JMP2r2
```

A **vector** is not called, so it ends in `BRK`. `JMP2r2` in a vector pops a
return address that was never pushed.

## Execution model

The host runs one instruction at a time and stops at `BRK`.

- The reset vector runs once, and installs the device vectors.
- Each frame: the File vector if armed, then the Controller vector if a key
  event is waiting, then the Mouse vector, then the Screen vector.
- Input is delivered first, so a program sees the position that caused it.
- The Console and audio are polled by the host, not run as vectors.

There is no interrupt mechanism and no priority: a vector runs to `BRK` or to
its step budget.

### Device page (`0x0100–0x01FF`)

16 devices × 16 ports. Device `N` occupies `0x0100 + N×16`.

| Addr | Device | Ports |
|---|---|---|
| `0x00` | System | wst, rst, red, green, blue, debug, state |
| `0x10` | Console | read, str, print, send, num, number, digit, newline |
| `0x20` | Screen | vector, width, height, auto, x, y, addr, pixel, sprite |
| `0x30` | Audio | vector, channel, waveform, period, volume, note, beat, envelope, phase |
| `0x40` | Palette | four shorts, one per colour |
| `0x50` | *(spare)* | |
| `0x60` | *(spare)* | |
| `0x70` | *(spare)* | |
| `0x80` | Controller | vector, key, button |
| `0x90` | Mouse | vector, x, y, state, scroll |
| `0xA0` | File | vector, result, count, op, status, name, data |
| `0xB0` | *(spare)* | |
| `0xC0` | Datetime | vector, year, month, day, hour, minute, second, weekday, yearday |
| `0xD0` | *(spare)* | |
| `0xE0` | *(reserved)* | |
| `0xF0` | *(reserved)* | |

A port the device does not claim behaves like a normal memory cell, so a
program can use scratch ports for its own purposes.

### System (`0x00`)

| Port | Meaning |
|---|---|
| `0x04` | working stack pointer, readable |
| `0x05` | return stack pointer, readable |
| `0x08`/`0x09`/`0x0a` | red, green, blue of colour 1, four bits each, read and write |
| `0x0e` | debug: write to print a line's worth of the program counter |
| `0x0f` | state: a program can leave a number here for a script to read |

The `0x08`/`0x09`/`0x0a` triple edits colour 1 one component at a time, which
is the cheapest way to animate a colour without a Palette write.

### Console (`0x10`)

| Port | Meaning |
|---|---|
| `0x12` | nonzero if a character is waiting to be read |
| `0x14`/`0x15` | a NUL-terminated string to print; any write starts it |
| `0x16` | print the string at `0x14` |
| `0x18` | send one byte |
| `0x1a`/`0x1b` | a number, set one byte at a time |
| `0x1c` | print that number in decimal |
| `0x1d` | print the byte on the stack as one digit |
| `0x1e` | newline; the value is ignored but one must be pushed |

### Screen (`0x20`)

| Port | Name | Notes |
|---|---|---|
| `0x20`/`0x21` | `vector` | run once per frame, 0 disables |
| `0x22`/`0x23` | `width` | readable, 320 |
| `0x24`/`0x25` | `height` | readable, 200 |
| `0x26` | `auto` | advance x after each draw, wrapping to the next row |
| `0x28`/`0x29` | `x` | cursor column |
| `0x2a`/`0x2b` | `y` | cursor row |
| `0x2c`/`0x2d` | `addr` | address of sprite data |
| `0x2e` | `pixel` | draw one pixel of the given colour |
| `0x2f` | `sprite` | draw a run of pixels |

### The sprite value byte

| Bits | Meaning |
|---|---|
| 0–1 | colour index, used when the flat bit is set |
| 2 | layer: 0 paints over anything, 1 keeps colour 0 so the background shows |
| 3 | flip horizontally |
| 4 | flip vertically (mirrors the run onto the row below) |
| 5–6 | length, plus one |
| 7 | flat: paint the mask in the colour above |

A sprite reads `length` **bytes** from `addr`, four pixels each, two bits per
pixel in the same order as the framebuffer: the high pair of bits is the
leftmost pixel. Zero is transparent; one, two or three is that colour.

Letting the data carry the colour is what makes a multi-colour sprite a single
pass. One call can draw three different colours; three calls would draw it
three times, once per colour, and each would need its own mask.

The flat bit paints every lit pixel in one colour instead, which is how a
shape is stamped and, with colour 0, erased. It is the reason there is no
separate "filled bar" mode: a solid run is just a mask byte of `$ff`, so the
extra mode bought nothing, and spending the bit on stamping buys the erase
that any moving sprite needs.

Two rules that cost an afternoon each:

- A sprite covers `length` bytes whether or not the shape uses them, so a mask
  table has to be padded out. Two bits per pixel means a six-pixel sprite is
  `$ff $f0`, not `$fc`: a byte holds four pixels, so six pixels is four lit
  pixels in the first byte and two in the next.
- The erase needs bit 7. Without it a "erase" simply repaints the sprite's own
  colours where it used to be, which is a trail rather than a clean erase.

Flip-y mirrors onto the row below, which is all a one-dimensional mask can
express; a taller sprite needs one call per row.

### Palette (`0x40`)

Four shorts, `$40` to `$47`, one per framebuffer colour, packed `0x0RGB` with
four bits each: red in bits 8–11, green in 4–7, blue in 0–3.

```
LIT2 $0fac
DEO2 $44                ; colour 2 is pink
```

### Audio (`0x30`)

Four channels. Each is a phase accumulator with an envelope, sampled by the
host — the SDL callback and the WAV writer share `audio.c`, so what you hear
is what a WAV file contains.

| Port | Meaning |
|---|---|
| `0x30`/`0x31` | vector, read to see if anything is queued |
| `0x32` | which channel the other ports address, 0–3 |
| `0x33` | waveform: 0 square, 1 triangle, 2 saw, 3 noise |
| `0x34`/`0x35` | period, samples per cycle, so pitch falls as it rises |
| `0x36`/`0x37` | volume, 0–0xffff |
| `0x38` | any write starts the note; zero stops it |
| `0x3a`/`0x3b` | attack time in milliseconds; 0 means none |
| `0x3c`/`0x3d` | the current envelope level, read only |
| `0x3e` | write restarts the phase and the envelope |

Two things to watch. `note` is an eight-bit port, so it must be written as a
byte: a short write puts the high half there, and a high half of zero is a
zero, which means "stop" rather than "play". And an eight-bit port pair cannot
be written as two independent bytes without ordering them; the short form is
the way.

### Controller (`0x80`) and Mouse (`0x90`)

Both are read-only, and both fire their vector once per event rather than once
per frame: a keyless frame does not re-run a key handler. A held key generates
one event, so a program that wants continuous control reads the key in its
frame vector, and the emulator's `-k` re-arms the key every frame to match.

| Port | Controller | Port | Mouse |
|---|---|---|---|
| `0x8c` | last key pressed | `0x92` | held buttons, `MB_*` bits |
| `0x8d` | held buttons plus bit 7 | `0x93`/`0x94` | x |
| | | `0x95`/`0x96` | y |
| | | `0x99`/`0x9a` | scroll |

Key codes are ASCII for printable keys, with low codes for the rest: `KEY_LEFT`
is `0x01`, `KEY_RIGHT` `0x02`, `KEY_UP` `0x03`, `KEY_DOWN` `0x04`, `KEY_SPACE`
`0x20`. Mouse position is clamped to the framebuffer.

### File (`0xA0`)

Eight slots of 512 bytes, held in a store file that is loaded at startup and
flushed at exit. Enough for a handful of saved games.

| Port | Meaning |
|---|---|
| `0xa0`/`0xa1` | vector |
| `0xa2`/`0xa3` | result: bytes read, or `0xffff` on failure |
| `0xa4`/`0xa5` | count: bytes written |
| `0xa6` | operation: 1 load, 2 save |
| `0xa7` | status: 0 ok, nonzero on error |
| `0xa8`/`0xa9` | pointer to a NUL-terminated name |
| `0xaa`/`0xab` | pointer to the data, or where to put it |

A name that is not in the store reads as zero bytes rather than an error, so a
first run needs no special case: zero means nobody has saved yet.

### Datetime (`0xC0`)

Read-only, sampled once per frame, so a program cannot busy-wait on it.

| Port | Meaning |
|---|---|
| `0xc0`/`0xc1` | vector |
| `0xc2`/`0xc3` | year, the full year rather than two digits |
| `0xc4` | month, 1–12 |
| `0xc5` | day, 1–31 |
| `0xc6` | hour, 0–23 |
| `0xc7` | minute |
| `0xc8` | second |
| `0xc9` | weekday, 0 Sunday |
| `0xca` | day of the year |

## Video

Framebuffer at `0x8000–0xBE7F`, 320×200 pixels, 2 bits per pixel, 4 colours
indexed into the 4-entry palette.

Pixel address: `(y × 320 + x) / 4` from `0x8000`.
Pixel offset within byte: `(x & 3) × 2`.

The graphics spare at `0xBE80–0xFFFF` is general RAM available for tile
patterns or sprite shapes. It is *not* scanned out by the display hardware.

## Permacomputing constraints

- CPU core in C89, one file, zero dependencies, no dynamic allocation.
- Two device callbacks (`dei`, `deo`) keep the core portable.
- The device layer is one implementation shared by both front ends.
- Every memory access decodes; address ranges are checked at run time, never
  assumed. Programs can crash if they write outside their intended region —
  acceptable, and it matches real 8-bit hardware.
- Soft reboot preserves the zero page. Hard reboot zeroes everything.

## The assembler

`duxasm` is a two-pass assembler. Pass 1 assigns addresses to labels and
records each line's byte length; pass 2 resolves references and emits code.
Instruction sizes are deterministic, so no backpatching is needed.

```sh
./duxasm -o test.rom asm/test.tal     # assemble
./duxasm -l -o test.rom asm/test.tal  # ... and report the size
./duxasm -S -o test.rom asm/test.tal # ... and list every symbol
```

### Syntax

| Form | Meaning |
|---|---|
| `@name` | label the current address |
| `@name = expr` | define a constant |
| `&name`, `$name` | reference a symbol |
| `&name` alone on a line | a label nested under the last `@name` |
| `$beef`, `1234` | numbers, `$` for hex |
| `;` | comment to end of line |
| `DB a, b, c` | emit bytes |
| `DW expr` | emit a 16-bit word, big-endian |
| `TEXT "hi\n"` | emit a string, escapes understood |
| `STRZ "hi"` | `TEXT` plus a NUL, which is what Console/print wants |
| `FILL n, v` | emit `n` copies of `v` |
| `INCLUDE "f.tal"` | assemble another file |
| `\|addr` | move the location counter |

Expressions are flat precedence, left associative, over `+ - * / >> <<`. `-` is
only an operator with spaces around it, because names may contain `-`; the
others need none. `$beef` is a number and `$ZP.cursor` is a symbol: `$` and `#`
introduce a number when a hex digit follows and a name otherwise.

Data belongs in `@name` tables, not `&name`. `&name` nests under the `@name`
routine it follows, which is what keeps one routine's jumps from colliding with
another's; a table wants a name of its own.

Data must come **after** the code. The assembler emits in source order and the
reset vector is fixed at `0x0200`, so anything ahead of the code would run
instead of it.

### Addresses are pushed for you

The assembler lowers operands into the correct push sequence, so the stack
conventions above never leak into your source:

```
LDA &x              ; push the address of x, then load a byte
LDA2 &x             ; ... and load a short
STA &x $00          ; write a byte to the address of x
DEO $18 $41         ; send a byte to device port $18
DEO2 $20 $0300      ; write a short to a device port
JSR &subr           ; absolute call
```

The one-operand forms take their value off the stack, and that distinction
matters more than it looks:

```
LDA &banner
LIT $01
ADD
STZ $ZP.cursor      ; correct: stores what is on the stack

STZ $ZP.cursor $00  ; also legal, and stores 0 -- leaving the sum underneath
```

### Two checks the assembler runs

Both exist because the failure they catch is silent: the program runs, draws
most of what it meant to, and misbehaves somewhere unrelated later.

**Zero page width.** The zero page is a byte array with no types, so nothing
stops a sixteen-bit counter and an eight-bit one living side by side. A byte
store into a short's high half is not an error at run time — the counter jumps
to 256 on its first step and any loop bounded by it either never runs or never
stops. The assembler records the width every symbolic zero-page operand was
used at and reports a name seen at both.

**Stack balance.** `asm/stackcheck.c` walks the emitted code from the reset
vector and from every device vector, carrying the depth of both stacks, and
reports an address reached with two different depths, a pop from a stack that is
not that deep, a push past the end, and a break with anything left over.

Following a branch is the interesting part. Targets are not read from the
instruction word — they are popped off the stack, because `JMP2 &label` assembles
to `LIT2 &label` followed by a jump that pops it — so the walk carries a shadow
of the stack holding the value of every byte whose value it knows. And a call's
effect on the stack has to be worked out rather than guessed: the callee is
walked first and its return depth becomes the caller's new depth, because
"LIT2 value / LIT port / JSR2 &put16" leaves two bytes that the routine then
consumes.

It is a checker, not a verifier. It counts bytes and has no types, so it cannot
tell a push of the wrong width from a push of the right one, and a branch whose
target it cannot pin down ends the path rather than guessing. Data is not
walked into: a jump into the middle of a sprite would report nonsense.

What it does catch includes the leak that is easiest to write by accident here:
`LDA2` puts two bytes on the stack and `STA` takes one, so stashing only the top
of a pair leaves the other underneath for the rest of the run. That is checked
at every depth — directly in a vector, one call down, and three calls down.

### Storing a short

`STA` takes an address and a value, in that order, and the assembler swaps them
on the way out so the value lands on top where the core wants it. So both of
these mean "put `$beef` at `$1000`":

```asm
        STA2 $1000 $beef
```

Read it the other way round and the address becomes the value, so `$1000` is
what ends up at `$beef` — which, if the address was meant to be somewhere
ordinary, is corruption a long way from the mistake. `tests_asm.sh` pins the
emitted bytes down.

## The self-check

`asm/test.tal` is the only program, and it is a test rather than a demo. It
exists because a machine this small can be wrong in ways that do not announce
themselves.

What makes it possible is that the framebuffer is not a mystery register: it is
ordinary memory at `$8000`. So a Dux program can draw, read the pixels back, and
compare them, and every video check here does exactly that. `@getpixel` computes
`$8000 + (y * 320 + x) / 4` and extracts two bits at a shift of `(x & 3) * 2`,
which is the part a two-bit framebuffer invites you to get wrong.

| Area | What is checked |
|---|---|
| Palette | each colour written and read back through the port pair |
| Geometry | width and height read back, since every address calculation assumes them |
| Pixel | a pixel drawn, read back, and its neighbour shown to have survived |
| Sprite | four pixels of one masked sprite read back one at a time, three colours in one call |
| Flat | a mask stamped in one colour, then in colour 0, which is the erase |
| Layer | a layer-1 sprite refuses the background and paints over anything else |
| Flip | a flipped run lands where the flip rule says it does, and not where it started |
| Bounds | a pixel past the right edge, a pixel past the bottom, and a sprite whose run overshoots: the graphics spare at `$be80` must be untouched after each |
| Cursor | the cursor pair reads back, and auto-advance moves x on by one |
| Audio | the note starts, the envelope restarts at zero, another channel is unaffected, zero stops it |
| Clock | every field is in range for the date |
| File | a value is saved, loaded into a different place, and compared; a name never written reads as zero bytes |

Failures print the check number and both values, because most of them are one
byte being one bit out and guessing which takes longer than printing it.

Two things it cannot check, stated plainly: the audio *samples*, which the host
makes rather than the core, so only the ports are verified; and the file
*format*, so only the round trip is. Both are covered from the C side instead.

The counters are left in the zero page for a host driving the machine directly:
pass at `$10`, fail at `$12`, two bytes each. `System/state` (`$0f`) is not a
status register — writing a nonzero byte to it asks the host to stop — so it can
only say that something went wrong, never how much.

## Status

Done: the core, seven devices (Screen, Palette, Console, Audio, Controller,
Mouse, File, Datetime), the shared device layer with a frame loop, two front
ends, and an assembler that checks the two mistakes this instruction set makes
easy.

Tests, all run by `make test`:

| Suite | Checks | What it covers |
|---|---|---|
| `tests` | 72 | the core, one instruction at a time |
| `tests-devices` | 63 | each device against the ports a program uses |
| `tests-host` | 71 | the frame loop, the store, the clock, the WAV and PPM writers |
| `tests_asm.sh` | 55 | emitted bytes, operand parsing, and the two static checks |
| `asm/test.tal` | 37 | the whole machine from the inside, drawing and reading back |

The self-check is not decoration. Writing it turned up a fill loop that never
advanced `y`, a sprite flag with the layer bit set where the length was meant,
an inverted branch in the check helper that made it fail everything that worked,
a byte-mode `EQU` comparing shorts, and a two-operand store with its operands
the wrong way round that was writing a pointer just past the framebuffer. Two
gaps in the C suite went with it: the short-mode comparisons had no coverage at
all, and the store operand order had none.

Outstanding: no scrolling hardware, so the video device is a framebuffer and a
cursor; no controller port for a second device; the File device's store is a
flat slot table with no way to enumerate it; and the self-check verifies the
audio and file devices only as far as their ports report, which is why the C
suites cover the samples and the on-disk format from outside.
