#!/usr/bin/env python3
"""Predict analog.rom's picture, pixel for pixel, so the count can be written
down before the ROM is run.

The model is deliberately written from the assembly rather than from the drawing
it is meant to check: same direction tables, same integer division, same
Bresenham. If the two disagree the ROM is wrong, and the disagreement is the
finding.

    python3 tools/clockmodel.py 10 9 36
    python3 tools/clockmodel.py 10 9 36 --ascii
"""
import sys

W, H = 320, 200
CX, CY = 160, 100
R_MARK_IN, R_MARK_OUT = 78, 88
R_HOUR, R_MIN, R_SEC = 46, 66, 76
WHITE, GREY153, GREY85 = 1, 2, 3
RGB = {0: (0, 0, 0), 1: (255, 255, 255), 2: (153, 153, 153), 3: (85, 85, 85)}


def table(n, step):
    """The biased direction tables, byte for byte as they appear in the ROM.

    A direction of -1 is 128, of +1 is about 255, and nothing is ever negative.
    The recovery for a hand of length r is byte * r / 128 - r.
    """
    import math
    out = []
    for k in range(n):
        th = math.radians(k * step)
        out.append((128 + round(127 * math.sin(th)), 128 - round(127 * math.cos(th))))
    return out


D5 = table(72, 5)
D6 = table(60, 6)


def offset(byte, r):
    """@offset: byte * r / 128 - r, with the machine's truncating divide."""
    return (byte * r) // 128 - r


def line(x0, y0, x1, y1):
    """The line walk in @line, with the error kept positive.

    The ends are *not* swapped and the roles of x and y are not exchanged: the
    loop picks its own axis. Swapping them, in any form, draws the transpose of
    the line wanted, which for a clock hand is a hand pointing somewhere else
    entirely.

    Note the `else` arms. The error loses the short axis on *every* step, not
    only on the step where the other axis moves; leaving it alone otherwise means
    it never changes again, so the short axis steps once and stops and every
    diagonal falls short of its far end.
    """
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 <= x1 else -1
    sy = 1 if y0 <= y1 else -1
    pts = []
    if dx >= dy:
        err = dx // 2
        while True:
            pts.append((x0, y0))
            if x0 == x1:
                return pts
            if err < dy:
                err = err - dy + dx
                y0 += sy
            else:
                err = err - dy
            x0 += sx
    else:
        err = dy // 2
        while True:
            pts.append((x0, y0))
            if y0 == y1:
                return pts
            if err < dx:
                err = err - dx + dy
                x0 += sx
            else:
                err = err - dx
            y0 += sy


def plot(px, x, y, colour):
    if 0 <= x < W and 0 <= y < H:
        px[(x, y)] = colour


def build(hour, minute, second):
    px = {}
    # the twelve marks, every fifth direction in the six-degree table
    for k in range(0, 60, 5):
        a, b = D6[k]
        plot(px, CX + offset(a, R_MARK_IN), CY + offset(b, R_MARK_IN), GREY153)
        plot(px, CX + offset(a, R_MARK_OUT), CY + offset(b, R_MARK_OUT), GREY153)
        line_i = (CX + offset(a, R_MARK_IN), CY + offset(b, R_MARK_IN))
        line_o = (CX + offset(a, R_MARK_OUT), CY + offset(b, R_MARK_OUT))
        for p in line(line_i[0], line_i[1], line_o[0], line_o[1]):
            plot(px, *p, GREY153)
    # the second hand, the longest and the darkest
    a, b = D6[second]
    for p in line(CX, CY, CX + offset(a, R_SEC), CY + offset(b, R_SEC)):
        plot(px, *p, GREY85)
    # the minute hand
    a, b = D6[minute]
    for p in line(CX, CY, CX + offset(a, R_MIN), CY + offset(b, R_MIN)):
        plot(px, *p, GREY153)
    # the hour hand, creeping through the hour in five-degree steps: six steps to
    # the hour (thirty degrees), and one more for every ten minutes (a minute is
    # half a degree, a tenth of a step).  At most 11*6 + 5 = 71, the last entry.
    k = ((hour % 12) * 6) + (minute // 10)
    a, b = D5[k]
    for p in line(CX, CY, CX + offset(a, R_HOUR), CY + offset(b, R_HOUR)):
        plot(px, *p, WHITE)
    # the middle, last, so it sits on top of all three
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            plot(px, CX + dx, CY + dy, WHITE)
    return px


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    hour, minute, second = (int(a) for a in args)
    px = build(hour, minute, second)
    lit = len(px)
    split = {}
    for c in px.values():
        split[c] = split.get(c, 0) + 1
    print('%d lit of %d' % (lit, W * H))
    print('  by colour: ' + '  '.join('c%d=%d' % (k, split[k]) for k in sorted(split)))
    if '--ascii' in sys.argv:
        ch = {1: 'H', 2: 'm', 3: 's'}
        xs = [p[0] for p in px]
        ys = [p[1] for p in px]
        for y in range(min(ys), max(ys) + 1, 2):
            print('  ' + ''.join(ch.get(px.get((x, y), 0), '.') for x in range(min(xs) - 2, max(xs) + 3)))


main()
