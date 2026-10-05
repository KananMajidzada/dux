#!/usr/bin/env python3
"""Print a box of a .ppm as ASCII, so a frame can be read by eye.

Usage:
    tools/ascii.py FILE.ppm X0 Y0 W H          one box,  '#' lit, '.' black
    tools/ascii.py FILE.ppm --boxes 110,126,150,166,190,206 9 8

Colours are shown by letter as well as by '#', when --colour is given:
white W, grey153 w, grey85 o, anything else ?.
"""
import sys


def load(path):
    d = open(path, 'rb').read()
    parts = []
    i = 0
    while len(parts) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    return int(parts[1]), int(parts[2]), d[i + 1:]


# the one ramp: white, then two greys
NAMES = {(0x0f, 0x0f, 0x0f): 'W', (0x09, 0x09, 0x09): 'w',
         (0x05, 0x05, 0x05): 'o'}


def main():
    a = sys.argv[1:]
    colour = '--colour' in a
    a = [x for x in a if x != '--colour']
    w, h, px = load(a[0])
    rest = a[1:]

    def show(x0, y0, bw, bh):
        for y in range(y0, min(y0 + bh, h)):
            if colour:
                row = ''.join(
                    NAMES.get((px[(y * w + x) * 3], px[(y * w + x) * 3 + 1],
                               px[(y * w + x) * 3 + 2]), '?')
                    if (px[(y * w + x) * 3], px[(y * w + x) * 3 + 1],
                        px[(y * w + x) * 3 + 2]) != (0, 0, 0) else '.'
                    for x in range(x0, min(x0 + bw, w)))
            else:
                row = ''.join('#' if (px[(y * w + x) * 3], px[(y * w + x) * 3 + 1],
                                       px[(y * w + x) * 3 + 2]) != (0, 0, 0) else '.'
                               for x in range(x0, min(x0 + bw, w)))
            print('    ' + row)
        print('    %d lit in this box' % sum(
            1 for y in range(y0, min(y0 + bh, h))
            for x in range(x0, min(x0 + bw, w))
            if (px[(y * w + x) * 3], px[(y * w + x) * 3 + 1],
                px[(y * w + x) * 3 + 2]) != (0, 0, 0)))

    if rest and rest[0] == '--boxes':
        xs = [int(x) for x in rest[1].split(',')]
        y0 = int(rest[2])
        bw = int(rest[3])
        bh = int(rest[4]) if len(rest) > 4 else 8
        for x0 in xs:
            print('  x=%d' % x0)
            show(x0, y0, bw, bh)
    else:
        show(int(rest[0]), int(rest[1]), int(rest[2]), int(rest[3]))


main()