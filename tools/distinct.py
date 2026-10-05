#!/usr/bin/env python3
"""distinct.py - every ROM in roms/ must draw a different picture.

    tools/distinct.py            check
    tools/distinct.py --quiet    print only on failure

Two identical programs satisfy the same recorded pixel count, so a duplicate is
the one defect the prediction table cannot see: it is not a wrong number, it is a
right number reached twice. This found two on the day it was written - `hstripe`
was `stripes2` with a different mask that happened to select the same rows, and
`blocks3` was byte-for-byte `blocks`, the second file having been overwritten by
the first.

Frames are hashed rather than the ROM, because two ROMs can differ in a way no
single frame shows: a different rotation, or a figure that moves. Four frames is
enough to separate the animated ones here and cheap enough to run every time.
"""

import hashlib
import os
import subprocess
import sys
import tempfile

FRAMES = '4'
DUPE = os.path.join(tempfile.gettempdir(), 'dux-distinct.ppm')


def picture(rom):
    """The md5 of `rom` rendered over FRAMES frames."""
    subprocess.run(['./dux', '-n', FRAMES, '-p', DUPE, rom], capture_output=True)
    with open(DUPE, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()[:12]


def main(argv):
    quiet = '--quiet' in argv
    roms = sorted('roms/' + f for f in os.listdir('roms') if f.endswith('.rom'))
    seen = {}
    dupes = []
    for r in roms:
        h = picture(r)
        name = os.path.basename(r)
        if h in seen:
            dupes.append((name, seen[h]))
        else:
            seen[h] = name
    if not quiet or dupes:
        print('%d ROMs, %d distinct pictures over %s frames'
              % (len(roms), len(seen), FRAMES))
    for a, b in dupes:
        print('  DUPLICATE %s draws exactly what %s draws' % (a, b))
    if os.path.exists(DUPE):
        os.unlink(DUPE)
    return 1 if dupes else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))