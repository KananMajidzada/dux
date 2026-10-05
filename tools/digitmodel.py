#!/usr/bin/env python3
"""Predict any frame of roms/clock.rom, the digital clock.

The ROM draws six seven-segment digits in a row at y = 90, then two colons
which blink on the top bit of the frame count. This reproduces the pixel set,
so it can be diffed against a rendered frame exactly rather than counted.

The geometry is the one in asm/clock.tal, and it has been wrong twice:

  - the bars were five pixels wide (`$ff, $c0`) with a one-pixel column
    (`$c0`), which is too fine to read; and
  - the bars were one pixel *tall* (sprite `$23`, no flip-y) standing next to
    a two-pixel column (sprite `$33`, flip-y), so a figure was twice as thick
    on its sides as on its bars.

Both are now `$ff, $ff` and `$f0`, and the bar sprite is `$33`, which makes the
digit exactly the 8x8 shape asm/pong.tal uses: bars eight wide and two rows,
columns two wide and four rows.

Usage:
    tools/digitmodel.py 1791102840            the count and the digit string
    tools/digitmodel.py 1791102840 --ascii    and the picture
    tools/digitmodel.py --times A B            every second from A to B
"""
import sys
import time

# Seven segments: bit, x, y, width, height, whether the sprite is horizontal.
SEGMENTS = [
    (0x01, 0, 0, 8, 2),   # a, top bar
    (0x02, 4, 0, 2, 4),   # b, top right
    (0x04, 4, 3, 2, 4),   # c, bottom right
    (0x08, 0, 6, 8, 2),   # d, bottom bar
    (0x10, 0, 3, 2, 4),   # e, bottom left
    (0x20, 0, 0, 2, 4),   # f, top left
    (0x40, 0, 3, 8, 2),   # g, middle bar
]

CODES = [0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f]

XS = (110, 126, 150, 166, 190, 206)     # the six digit origins
Y = 90
COLON_X = (140, 180)                    # and the two colons between them
COLON_Y = (92, 97)                      # a colon is two 2x2 dots


def digit(v, x, y):
    """The pixel set for one digit value."""
    px = set()
    for bit, sx, sy, w, h in SEGMENTS:
        if not (CODES[v] & bit):
            continue
        for dx in range(w):
            for dy in range(h):
                px.add((x + sx + dx, y + sy + dy))
    return px


def frame(epoch, frames=None):
    """The whole picture. `frames` is the ROM's frame counter, which decides
    the colon blink: it is off when bit 5 is set, i.e. for 32 frames on and
    32 off."""
    # clock.c reads the device with localtime(), not gmtime(), so the model has
    # to as well or every hour is wrong.
    t = time.localtime(epoch)
    values = (t.tm_hour // 10, t.tm_hour % 10,
              t.tm_min // 10, t.tm_min % 10,
              t.tm_sec // 10, t.tm_sec % 10)
    if frames is None:
        frames = 1
    px = set()
    for x0, v in zip(XS, values):
        px |= digit(v, x0, Y)
    if not frames & 0x20:
        for cx in COLON_X:
            for cy in COLON_Y:
                for dx in range(2):
                    for dy in range(2):
                        px.add((cx + dx, cy + dy))
    return px, values


def count(epoch, frames=None):
    return len(frame(epoch, frames)[0])


def main():
    a = sys.argv[1:]
    if a and a[0] == '--times':
        lo, hi = int(a[1]), int(a[2])
        seen = {}
        for e in range(lo, hi):
            seen.setdefault(count(e), []).append(e)
        for c in sorted(seen):
            print('  %3d lit  %4d second(s), first at %d'
                  % (c, len(seen[c]), seen[c][0]))
        return
    epoch = int(a[0]) if a else int(time.time())
    frames = int(a[1]) if len(a) > 1 else 1
    px, values = frame(epoch, frames)
    t = time.strftime('%H:%M:%S', time.localtime(epoch))
    print('%s  digits %s  colons %s  %d lit'
          % (t, ''.join(map(str, values)),
             'on' if not frames & 0x20 else 'off', len(px)))
    if '--ascii' in a:
        xs = range(min(XS) - 2, max(XS) + 10)
        for y in range(Y - 1, Y + 11):
            print('    ' + ''.join('#' if (x, y) in px else '.' for x in xs))


if __name__ == '__main__':
    main()