# Dux - an 8-bit stack machine.
#
# Plain C89, no dependencies in the core. The core (dux.c) knows nothing
# about the host; the CLI (duxemu.c) holds every platform-specific call.
#
#   make            build libdux.a and the dux emulator
#   make test       build and run the conformance suite
#   make asan       rerun the suite under AddressSanitizer
#   make debug      rerun the suite with assertions, no optimisation

CC      ?= cc
CFLAGS  ?= -std=c89 -Wall -Wextra -pedantic -Os
LDFLAGS ?=

CORE    = dux.c dux.h
DEVS    = host.c screen.c audio.c clock.c file.c dux.c \
          host.h screen.h audio.h clock.h file.h
CLI     = duxemu.c $(DEVS)
SDL     = duxsdl.c $(DEVS)
TESTS   = tests.c dux.c dux.h
DEVT    = tests_devices.c screen.c dux.c dux.h screen.h
HOSTT   = tests_host.c $(DEVS)
LIB     = libdux.a
BIN     = dux
ASM     = duxasm
GUX     = duxsdl

# SDL2 is optional. `make` builds the headless tools; `make sdl` adds the
# windowed front end, and is skipped cleanly when SDL2 is not installed.
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)
HAVE_SDL   := $(shell sdl2-config --version 2>/dev/null)

.PHONY: all sdl smoke crosscheck test tests tests-devices tests-host tests-asm selftest crosscheck roms clean debug asan

all: $(LIB) $(BIN) $(ASM)

sdl: $(GUX)

# The library: CPU core only, safe to link into a freestanding target.
$(LIB): dux.c dux.h
	$(CC) $(CFLAGS) -c dux.c -o dux.o
	ar rcs $@ dux.o

# The emulator: core plus the CLI host.
$(BIN): $(CLI)
	$(CC) $(CFLAGS) -o $@ duxemu.c $(DEVS) $(LDFLAGS)

# The SDL front end. Skipped if SDL2 is missing, since the headless tools
# cover everything else.
$(GUX): $(SDL) dux.h
ifeq ($(HAVE_SDL),)
	@echo "note: SDL2 not found, skipping $(GUX)"
else
	$(CC) -std=c89 -Wall -Wextra -Os $(SDL_CFLAGS) -o $@ \
	      duxsdl.c $(DEVS) $(LDFLAGS) $(SDL_LIBS)
endif

# The assembler: two-pass, standalone, links only against dux.h for constants.
# stackcheck.c walks the emitted code looking for unbalanced paths, so it comes
# along rather than living inside the assembler.
$(ASM): asm/duxasm.c asm/stackcheck.c asm/stackcheck.h dux.h
	$(CC) $(CFLAGS) -o $@ asm/duxasm.c asm/stackcheck.c

test: tests tests-devices tests-host tests-asm selftest

tests: $(TESTS)
	$(CC) $(CFLAGS) -o $@ tests.c dux.c $(LDFLAGS)
	./tests

# Device tests link the real screen device, not a stand-in.
tests-devices: $(DEVT)
	$(CC) $(CFLAGS) -o $@ tests_devices.c screen.c dux.c $(LDFLAGS)
	./$@

# Host tests cover input queueing and the frame loop.
tests-host: $(HOSTT)
	$(CC) $(CFLAGS) -o $@ tests_host.c $(DEVS) $(LDFLAGS)
	./$@

# Assembler tests compare emitted bytes, so they drive the binary itself.
tests-asm: $(ASM)
	./tests_asm.sh

# The self-check, which is a Dux program that exercises the machine from the
# inside: it draws, reads the framebuffer back out of memory, and compares.
# The check is the printed count, not the exit status, because a Dux program
# has no way to set one.
selftest: $(ASM) $(BIN)
	./$(ASM) -o /tmp/dux-selftest.rom asm/test.tal
	@out=$$(./$(BIN) -n 3 -s /tmp/dux-selftest.store \
	         /tmp/dux-selftest.rom); \
	printf '%s\n' "$$out"; \
	printf '%s' "$$out" | grep -q '[0-9] failed' || \
	    { echo "self test printed no result"; exit 1; }; \
	printf '%s' "$$out" | grep -q ', 0 failed' || \
	    { echo "self test FAILED"; exit 1; }; \
	echo "self test ok"

# Assemble every example. Cheap, and it catches a broken build before the
# smoke test has to notice.
# Cross-check the two front ends on the same ROM. They share the core and the
# device layer and differ only in how they get pixels onto a screen, which is
# exactly where a mirrored framebuffer hides: one front end looked perfect while
# the other drew garbage. Comparing the two dumps catches that class outright.
crosscheck: $(BIN) $(GUX) $(ASM)
	@mkdir -p roms
	@./$(ASM) -o roms/test.rom asm/test.tal
	@./$(BIN) -n 3 -p /tmp/dux-a.ppm roms/test.rom >/dev/null 2>&1
	@SDL_VIDEODRIVER=dummy ./$(GUX) -m -F 3 -S /tmp/dux-b.ppm roms/test.rom >/dev/null 2>&1
	@cmp /tmp/dux-a.ppm /tmp/dux-b.ppm \
	    && echo "dux and duxsdl agree on test.rom" \
	    || { echo "FRONT ENDS DISAGREE"; exit 1; }

# Assemble every program and run it, because a ROM that assembles and then
# traps on its first frame is not a ROM. The self-check prints its own result,
# so its "ok" here means it ran, not that it passed: make test covers that.
roms: $(ASM) $(BIN)
	@mkdir -p roms
	@for f in asm/*.tal; do \
		case $$f in *common.tal) continue;; esac; \
		n=`basename $$f .tal`; \
		printf '  roms/%-12s' "$$n.rom"; \
		./$(ASM) -X -o roms/$$n.rom "$$f" || { echo "  FAILED"; exit 1; }; \
		printf '%7d bytes' `wc -c < roms/$$n.rom`; \
		warn=`./$(BIN) -n 3 roms/$$n.rom 2>&1 >/dev/null | grep -v '^dux: '`; \
		if [ -n "$$warn" ]; then \
			echo "  WARN"; echo "$$warn" | sed 's/^/      /'; \
		else \
			echo "  ok"; \
		fi; \
	done

# Bounds and integer checks. The core should be clean here; if it is not,
# a real bug exists rather than a stylistic one.
asan:
	$(CC) -std=c89 -Wall -Wextra -g -fsanitize=address,undefined \
	      -o tests-asan tests.c dux.c
	./tests-asan
	$(CC) -std=c89 -Wall -Wextra -g -fsanitize=address,undefined \
	      -o tests-dev-asan tests_devices.c screen.c dux.c
	./tests-dev-asan
	$(CC) -std=c89 -Wall -Wextra -g -fsanitize=address,undefined \
	      -o tests-host-asan tests_host.c $(DEVS)
	./tests-host-asan

# Unoptimised build, for stepping through in a debugger.
debug: tests.c dux.c dux.h
	$(CC) -std=c89 -Wall -Wextra -g -O0 -o tests-dbg tests.c dux.c
	./tests-dbg

# Smoke test the SDL front end without a display. The self-check is what runs
# here: it is the only program left, it puts four colours on screen and then
# reads them back, and it exercises every device on the way. The frame it leaves
# is checked for several distinct colours, so a blank or all-background frame
# fails instead of being mistaken for success.
ifeq ($(HAVE_SDL),)
smoke:
	@echo "note: SDL2 not found, skipping smoke test"
else
smoke: $(GUX)
	./$(ASM) -o tests_smoke.rom asm/test.tal
	@SDL_VIDEODRIVER=dummy ./$(GUX) -m -F 3 -S tests_smoke.ppm tests_smoke.rom \
	    > tests_smoke.out 2>&1 || { cat tests_smoke.out; exit 1; }
	@cat tests_smoke.out
	@grep -q ', 0 failed' tests_smoke.out || \
	    { echo "smoke: the self-check reported a failure"; exit 1; }
	@python3 -c "import sys; \
	    from collections import Counter; \
	    d=open('tests_smoke.ppm','rb').read(); \
	    i=d.index(b'255\n')+4; px=d[i:]; \
	    c=Counter(tuple(px[k:k+3]) for k in range(0,len(px),3)); \
	    bg,n=c.most_common(1)[0]; \
	    drawn=len(px)//3-n; \
	    print('colours: %d, drawn: %d of %d' % (len(c), drawn, len(px)//3)); \
	    sys.exit(0 if len(c) >= 4 and drawn >= 10 else 1)"
endif

clean:
	rm -f $(BIN) $(ASM) $(GUX) tests tests-devices tests-host tests-dbg tests-asan \
	      tests-dev-asan tests-host-asan roms/test.rom tests_smoke.rom tests_smoke.ppm tests_smoke.out dux.o $(LIB)
verify:
	./tools/verify.sh
	./tools/balcheck.sh
	./tools/distinct.py --quiet
	./tools/probe.sh
	@echo "stack-checking every ROM again, so a warning cannot hide here"
	@for f in asm/*.tal; do \
		case $$f in *common.tal) continue;; esac; \
		./$(ASM) -X -o /dev/null "$$f" >/dev/null 2>/tmp/duxasm.$$$$ || \
			{ echo "  FAILED $$f"; cat /tmp/duxasm.$$$$; exit 1; }; \
	done
	@rm -f /tmp/duxasm.*; echo "every ROM stack-checks clean"
