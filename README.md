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
