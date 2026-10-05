#!/usr/bin/env python3
"""Check a rendered frame against a prediction.

    ppmcheck.py frame.ppm [expected_pixels] [expected_colours]

Predict the pixel count *before* you render and pass it in. A wrong count is the
cheapest bug report there is: it says a loop ran the wrong number of times, or
that half of what you drew is invisible, before you go looking at pictures.

Two things this does that the ad-hoc version got wrong:

The background is the **most common colour**, not a sample of one corner. A
drawing that reaches the corner - which is exactly what "drawn flush to the
origin" means - makes a corner sample land on the drawing, and then every one
of the background pixels reads as lit. That reports a runaway loop on a picture
that is entirely correct. Twice, on this project, before the check was fixed.

The count is a count of the **union**, not of the plot calls. Overlapping pixels
count once. A prediction has to be made the same way: 100 cells of 64 pixels is
6400 only if no two cells touch.

Exit status is 0 when the prediction holds, 1 when it does not, so a Makefile
can depend on it.
"""

import sys
from collections import Counter


def read_ppm(path):
    data = open(path, "rb").read()
    # Header is "P6\n<w> <h>\n<max>\n", whitespace separated, max is 255.
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
    i += 1                                            # single whitespace byte
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i:]


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    w, h, px = read_ppm(argv[1])

    def rgb(x, y):
        k = (y * w + x) * 3
        return (px[k], px[k + 1], px[k + 2])

    histogram = Counter(rgb(x, y) for y in range(h) for x in range(w))
    bg, bg_count = histogram.most_common(1)[0]

    # The background is the most common colour, and when that is not true the
    # answer is given rather than guessed: --bg R,G,B.
    #
    # Guessing is how this went wrong twice. A corner sample breaks on anything
    # drawn flush to the origin, and a majority rule breaks on a drawing that
    # covers more of the screen than the ground - quad covers 76% of it, so the
    # largest colour there is paint. A corner heuristic breaks on a border,
    # which is the one thing that does reach the corners. All three were tried
    # and all three were wrong somewhere.
    bg_arg = None
    for a in argv[2:]:
        if a.startswith("--bg="):
            bg_arg = tuple(int(p) for p in a[5:].split(","))
    if bg_arg is not None:
        bg = bg_arg
        bg_count = histogram.get(bg, 0)
        guessed = "given on the command line"
    else:
        guessed = "most common colour, no --bg given"
    lit = [(x, y) for y in range(h) for x in range(w) if rgb(x, y) != bg]
    colours = sorted({rgb(x, y) for x, y in lit})

    name = argv[1].rsplit("/", 1)[-1]
    print("%-18s %d lit of %d, %d colours" % (name, len(lit), w * h, len(colours)))
    print("%-18s background taken as %s, %d pixels (%.0f%% of the screen, %s)"
          % ("", bg, bg_count, 100.0 * bg_count / (w * h), guessed))
    # Always print the whole histogram, whatever the background was taken as.
    # Which colour is "the background" is a guess, and a guess that is wrong
    # silently turns a correct picture into a wrong count - which has happened
    # here twice, once in each direction. The histogram is not a guess.
    print("%-18s every colour on screen:" % "")
    for c, n in sorted(histogram.items(), key=lambda kv: -kv[1]):
        mark = " <- taken as background" if c == bg else ""
        print("%-18s   %-16s %7d%s" % ("", c, n, mark))
    if lit:
        xs = [p[0] for p in lit]
        ys = [p[1] for p in lit]
        print("%-18s extent x %d..%d  y %d..%d" % ("", min(xs), max(xs), min(ys), max(ys)))

    # A screen that is entirely one colour is ambiguous: it is what a program
    # that draws nothing looks like, and also what a program whose predicate is
    # always true looks like. `xmark` reported "0 lit of 64000" and meant the
    # second of those.
    if len(lit) == 0 and len(histogram) <= 2:
        print("%-18s WARNING  the whole screen is %s - either nothing was drawn"
              % ("", bg))
        print("%-18s          or the predicate is true everywhere. A count of"
              % "")
        print("%-18s          zero cannot tell those apart." % "")

    bad = []
    pos = [a for a in argv[2:] if not a.startswith("--bg=")]
    if len(pos) > 0 and int(pos[0]) != len(lit):
        bad.append("pixels: want %s, got %d" % (pos[0], len(lit)))
    if len(pos) > 1 and int(pos[1]) != len(colours):
        bad.append("colours: want %s, got %d" % (pos[1], len(colours)))

    for b in bad:
        print("%-18s FAIL  %s" % ("", b))
    if not bad and len(argv) > 2:
        print("%-18s ok    prediction held" % "")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))