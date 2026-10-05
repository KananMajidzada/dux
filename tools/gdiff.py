#!/usr/bin/env python3
"""Compare a rendered frame against tools/gearsmodel.py, pixel by pixel.

    gdiff.py frame.ppm [n]

An equal pixel *count* is not agreement. 1166 wrong pixels and 1166 right
pixels both count 1166, and the gears have three separate parts that can each
be missing while the others make up the difference. So this reports, per gear
and per part, which pixels the ROM drew that the model did not, and which the
model drew that the ROM did not - and an equal count with a symmetric
difference is called out on its own, because that is the one case a count
cannot see.
"""

import sys

sys.path.insert(0, "tools")
import gearsmodel as g


def read_ppm(path):
    """The lit pixels, with the header parsed the way ppmcheck parses it.

    Slicing on the bytes of the maxval and taking everything after is the
    obvious way to find the pixel data and it is wrong by four bytes: the
    "255\n" that follows the slice point is itself part of what you kept, so
    every pixel read is shifted and the whole picture slides. The counts still
    look plausible, which is what makes it dangerous - 380 read against 339
    real, on the same file, from the same run.
    """
    data = open(path, "rb").read()
    i, fields = 0, []
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":                     # comment to end of line
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1                                         # one whitespace byte past maxval
    w, h = int(fields[1]), int(fields[2])
    px = data[i:]
    lit = set()
    for y in range(h):
        for x in range(w):
            if px[(y * w + x) * 3:(y * w + x) * 3 + 3] != b"\x00\x00\x00":
                lit.add((x, y))
    return lit


def parts(n):
    """The model's per-gear, per-part pixel sets."""
    out = []
    for gi, gear in enumerate(g.GEARS):
        cx, cy = gear[0], gear[1]
        npts = g.npts(gear)
        pts = [(cx + dx, cy + dy) for dx, dy in g.offsets(gi, npts)]
        own = set()
        for k in range(npts):
            own |= set(g.line(pts[k], pts[(k + 1) % npts]))
        spoke = set()
        step = max(1, npts // g.SPOKES)
        for k in range(0, npts, step):
            spoke |= set(g.line((cx, cy), pts[k]))
        hub = set(g.disc((cx, cy), g.HUB))
        out.append((own, spoke, hub))
    return out


def main():
    path = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    got = read_ppm(path)
    want, counts = g.frame(n)
    mine = parts(n)

    print("ROM %d lit, model %d lit" % (len(got), len(want)))
    extra = got - want
    missing = want - got
    if not extra and not missing:
        print("IDENTICAL, pixel for pixel")
        return 0
    if len(extra) == len(missing) and not extra:
        pass
    print("  ROM drew %d the model does not have" % len(extra))
    print("  model has %d the ROM does not draw" % len(missing))
    if len(got) == len(want) and len(extra) == len(missing):
        print("  ** the counts agree and the pixels do not - a count alone would "
              "have passed this **")

    names = ("outline", "spoke", "hub")
    for gi, (own, spoke, hub) in enumerate(mine):
        gear = g.GEARS[gi]
        print("gear %d at %s, %d teeth, r=%d, %d points"
              % (gi + 1, (gear[0], gear[1]), gear[4], gear[2], g.npts(gear)))
        for nm, ps in zip(names, (own, spoke, hub)):
            has = len(ps & got)
            print("    %-8s model %4d, ROM %4d%s"
                  % (nm, len(ps), has,
                     "" if has == len(ps) else "   <-- %d missing" % (len(ps) - has)))
    return 1


if __name__ == "__main__":
    sys.exit(main())