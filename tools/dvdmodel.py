#!/usr/bin/env python3
"""Predict any frame of roms/bounce.rom, the DVD logo.

Reproduces the logo's position, its colour and its whole pixel set, so a render
can be diffed against it exactly instead of counted. The count is 1011 on every
frame by construction - the logo is 51 x 38 and is reflected into x 0..269 and
y 0..162, so it can never hang off the screen - which is why the model predicts
the picture and the picture is the test.

The movement is a step per axis, one to three pixels, re-rolled at every bounce,
with the sign coming from which wall was hit. The old version held one step pair
for the whole run and flipped only the signs, so it could travel four ways; see
the notes at the head of asm/bounce.tal.

Usage:
    tools/dvdmodel.py 1000            the last of 1000 frames
    tools/dvdmodel.py 1000 --trace    every bounce in the first 1000 frames
    tools/dvdmodel.py --speeds 5000   the distinct step pairs seen
"""
import sys

MARK = [0x7f, 0x41, 0x41, 0x41, 0x3e, 0x00, 0x1f, 0x20, 0x40, 0x20, 0x1f,
        0x00, 0x7f, 0x41, 0x41, 0x41, 0x3e]
OVAL = [9, 15, 18, 21, 22, 24, 24, 25, 24, 24, 22, 21, 18, 15, 9]

MAGS = [1, 2, 3, 2, 3, 1, 3, 2]      # @mags in asm/bounce.tal
MAX_X = 269                          # 269 + 50 is the last column on screen
MAX_Y = 162                          # 162 + 37 is the last row on screen
START_X, START_Y = 30, 30
SEED = 1


class Rng:
    """s <- 5s + 1 mod 65536, handing back the high byte. A short keeps its high
    byte at the lower address, so that is where the byte @rnd reads lives."""

    def __init__(self, s=SEED):
        self.s = s

    def byte(self):
        self.s = (5 * self.s + 1) & 0xffff
        return (self.s >> 8) & 0xff

    def mag(self):
        return MAGS[self.byte() & 7]


def pixels(x, y):
    """Every lit pixel of the logo with its top left at (x, y): the wordmark as
    three-by-three blocks, then one row per disc row."""
    px = set()
    for col, mask in enumerate(MARK):
        for row in range(7):
            if mask >> row & 1:
                for dx in range(3):
                    for dy in range(3):
                        px.add((x + col * 3 + dx, y + row * 3 + dy))
    for row, half in enumerate(OVAL):
        yy = y + 23 + row
        for xx in range(x + 25 - half, x + 25 + half + 1):
            px.add((xx, yy))
    return px


def _step(pos, delta, limit):
    """Move one axis and reflect. The reflection is done on the *sum*, which is
    what @step-x does - it stores x + dx and then subtracts from zero, so the
    overshoot comes back the right way round. Returns the new position, the new
    direction (1 or -1) and whether a wall was hit."""
    total = pos + delta
    if total < 0:
        return -total, 1, True
    if total > limit:
        return limit - (total - limit), -1, True
    return total, (1 if delta > 0 else -1), False


def frames(n):
    """Walk n frames, yielding (x, y, colour, dx, dy, hit_x, hit_y)."""
    x, y = START_X, START_Y
    rng = Rng()
    dx, dy = rng.mag(), rng.mag()
    colour = 1
    out = []
    for _ in range(n):
        x, sx, hit_x = _step(x, dx, MAX_X)
        y, sy, hit_y = _step(y, dy, MAX_Y)
        if hit_x:
            dx = sx * rng.mag()
        if hit_y:
            dy = sy * rng.mag()
        if hit_x and hit_y:            # a corner, and the only place colour moves
            colour = 1 if colour > 3 else colour + 1
        out.append((x, y, colour, dx, dy, hit_x, hit_y))
    return out


def frame(n):
    """The (x, y, colour) of the nth frame, counting the reset draw as frame 1."""
    return frames(n)[:n][-1] if n else None


def main():
    a = sys.argv[1:]
    n = int(a[0]) if a else 1000
    seq = frames(n)
    if '--speeds' in a:
        seen = {}
        for _, _, _, dx, dy, _, _ in seq:
            seen[(dx, dy)] = seen.get((dx, dy), 0) + 1
        print('%d frames, %d distinct step pairs, all within 1..3: %s'
              % (n, len(seen), all(1 <= abs(v) <= 3 for p in seen for v in p)))
        print('  ' + '  '.join('%+d%+d:%d' % (dx, dy, c)
                               for (dx, dy), c in sorted(seen.items())))
        return
    bounces = [(i + 1, x, y, dx, dy, 'corner' if hx and hy else
                ('x' if hx else 'y'))
               for i, (x, y, _, dx, dy, hx, hy) in enumerate(seq) if hx or hy]
    print('%d frames, %d bounces, %d corners'
          % (n, len(bounces), sum(1 for b in bounces if b[5] == 'corner')))
    if '--trace' in a:
        for b in bounces[:60]:
            print('   frame %4d  at %3d,%3d  next step %+d%+d  %s'
                  % (b[0], b[1], b[2], b[3], b[4], b[5]))
    x, y, colour = seq[-1][:3]
    print('last frame: x=%d y=%d colour=%d  %d lit'
          % (x, y, colour, len(pixels(x, y))))


if __name__ == '__main__':
    main()