"""show.py - print a region of a PPM as text, one character a pixel.

    python3 tools/show.py /tmp/clock.ppm [x0 x1 y0 y1]

'#' is a lit pixel, '.' is not. Handy for looking at what a ROM actually drew
without a window open: the digit shapes are the whole point of a clock ROM, and
reading them off a rendered picture is the only check that catches a glyph that
is the wrong glyph.
"""
import sys

sys.path.insert(0, '/home/kanan/vm/tools')
from ppmcheck import read_ppm

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    w, h, d = read_ppm(sys.argv[1])
    x0, x1, y0, y1 = 0, w, 0, h
    if len(sys.argv) >= 6:
        x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6])
    for y in range(y0, min(y1, h)):
        row = []
        for x in range(x0, min(x1, w)):
            k = (y * w + x) * 3
            row.append('#' if d[k:k + 3] != b'\x00\x00\x00' else '.')
        print(''.join(row))

main()
