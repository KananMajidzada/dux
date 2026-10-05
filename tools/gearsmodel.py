#!/usr/bin/env python3
"""Predict the gears frame exactly, pixel for pixel.

    gearsmodel.py [frame] [--tables]

Three gears turning in mesh: a toothed outline, a hub, and spokes. Eight points
to a tooth, walked as line segments so the flanks come out straight.

Why the numbers are what they are. Four things have to hold at once, and three
of them were wrong in the first version of this file:

  * The pitch radius is proportional to the tooth count, or one gear's tooth is
    wider than the other's gap and they cannot mesh. Radii 24, 16 and 32 for
    12, 8 and 16 teeth: 3:2:4, the same ratio.

  * Two gears in mesh roll without slipping, so omega_a * r_a = -omega_b * r_b.
    The first version asserted instead that rate * teeth must match, and with
    a tooth two table points that meant equal |rate|, which gives omega
    proportional to 1/teeth and omega*r *proportional to r* - unequal, so the
    gears were slipping and no starting phase could ever hold. The correct
    reading is the reverse: because r is already proportional to teeth, equal
    |rate| is exactly what makes them roll. Rates +1, -1, +1.

  * One slot a frame, which is 1/8 of a tooth pitch. The old rates were whole
    teeth, and a gear's figure repeats every whole tooth pitch, so those rates
    drew the same pixels forever. That is the real reason nothing moved, and no
    choice of starting phase would have shown it.

  * The two profiles are offset half a pitch at each mesh, and with a symmetric
    tooth that makes the contact sum *constant*, not merely invariant: gear A's
    contact slot advances one a frame and gear B's goes back one, so their sum
    never moves, and h[s] + h[(4-s) mod 8] is the same for every s. Meshing
    therefore needs no per-frame checking at all - interlock() checks it anyway,
    because "needs no checking" is exactly what a wrong derivation claims.

The tables are this model's output, and asm/gears.tal reads precomputed offsets
for all eight phases rather than working a direction out at run time. That is
deliberate: a run-time multiply, divide and unsigned bias is three more places
for the ROM and the model to disagree by a pixel, and phase tables cost 2304
bytes instead of a rounding rule.
"""

import math
import sys

W, H = 320, 200

# The tooth profile, as eight heights. Root, rising flank, flat tip, falling
# flank: h[s] = 0 is the gap floor and h[s] = 8 the tip. Symmetric about the
# middle of the tooth, which is what makes the contact sum constant.
PROF = [0, 1, 2, 3, 4, 3, 2, 1]
PPTOOTH = len(PROF)               # eight points a tooth

# centre x, centre y, root radius, tooth depth, teeth, rate in slots a frame
GEARS = [
    (110, 100, 24, 4, 12, +1),
    (152, 100, 16, 4, 8, -1),
    (202, 100, 32, 4, 16, +1),
]

HUB = 6                           # the hub's radius, and where a spoke starts
SPOKES = 6                        # every n/SPOKES-th point


def npts(g):
    return PPTOOTH * g[4]


def radius(g, slot):
    """The outline radius at a given slot of the tooth profile."""
    return g[2] + g[3] * PROF[slot % PPTOOTH] / 8.0


def cos_table(g):
    """This gear's direction table: index i is the direction at i*360/n degrees.

    Units are 1000 so the ROM can read a precomputed offset with no multiply
    and no divide. Straight up is index 0, and the table runs clockwise, which
    is the screen's y growing downwards.
    """
    n = npts(g)
    return [(round(1000 * math.cos(2 * math.pi * i / n)),
             round(1000 * math.sin(2 * math.pi * i / n))) for i in range(n)]


def bearing(g, dx, dy):
    """The index of the direction nearest to the vector (dx, dy), in g's table.

    The angles differ between gears because the tables have different lengths,
    so a bearing has to be worked out in each gear's own index space.
    """
    n = npts(g)
    ang = math.atan2(dy, dx) % (2 * math.pi)
    return round(ang / (2 * math.pi) * n) % n


def mesh_bearings(a, b):
    """Each gear's table index at the point where the two pitch circles touch."""
    ga, gb = GEARS[a], GEARS[b]
    dx, dy = gb[0] - ga[0], gb[1] - ga[1]
    return bearing(ga, dx, dy), bearing(gb, -dx, -dy)


def contact_sum(a, b, sa, sb):
    """How far the two outlines reach towards each other, summed.

    This is the whole collision test: if it exceeds the distance between the
    centres the teeth are inside one another.
    """
    return radius(GEARS[a], sa) + radius(GEARS[b], sb)


def interlock(n):
    """Do the teeth stay meshed, or only look like they touch?"""
    bad = []
    for a, b in ((0, 1), (1, 2)):
        ba, bb = mesh_bearings(a, b)
        dist = math.hypot(GEARS[b][0] - GEARS[a][0], GEARS[b][1] - GEARS[a][1])
        for f in range(64):
            ia = (phase_of(a, f) + ba) % PPTOOTH
            ib = (phase_of(b, f) + bb) % PPTOOTH
            if contact_sum(a, b, ia, ib) > dist + 1e-9:
                bad.append("gears %d,%d at frame %d: teeth overlap by %.2f "
                           "(sum %.2f > centres %.2f apart)"
                           % (a + 1, b + 1, f, contact_sum(a, b, ia, ib) - dist,
                              contact_sum(a, b, ia, ib), dist))
                break
    return bad


def mesh_phases():
    """The starting phase each gear needs, so every mesh pair is tip-to-gap.

    The two profiles are offset by half a pitch - four slots - at each contact.
    Both offsets together have to land on the two meshes at once, which is one
    free choice per mesh against two gears' worth of phase, so it is searched
    rather than solved.
    """
    bearings = [mesh_bearings(0, 1), mesh_bearings(1, 2)]
    half = PPTOOTH // 2
    for a in range(PPTOOTH):
        for b in range(PPTOOTH):
            for c in range(PPTOOTH):
                cand = [a, b, c]
                ok = True
                for (i, j), (bi, bj) in zip(((0, 1), (1, 2)), bearings):
                    si = (cand[i] + bi) % PPTOOTH
                    sj = (cand[j] + bj) % PPTOOTH
                    # Tip into gap: the two profiles a half pitch apart, so the
                    # one gear's tip meets the other's floor.
                    if (si - sj) % PPTOOTH != half:
                        ok = False
                        break
                if ok:
                    return cand
    raise ValueError("no phase set meshes all three gears tip-to-gap")


MESH = mesh_phases()


def phase_of(gi, frame):
    return (MESH[gi] + GEARS[gi][5] * frame) % PPTOOTH


def offsets_at_phase(gi, phase):
    """The star's points about the centre at a given phase.

    The phase and the frame number are different things and the difference
    matters here. The ROM picks a table by phase; the animation is indexed by
    frame. For gear 1 they coincide (MESH 0, rate +1) and for gear 3 too, so
    only gear 2 - MESH 4, rate -1 - catches you: emitting tables by frame there
    hands the ROM exactly the wrong one, and the gears run backwards.
    """
    g = GEARS[gi]
    n = npts(g)
    ctab = cos_table(g)
    out = []
    for k in range(n):
        # The point's angle is its identity. The phase says which slot of the
        # tooth profile it currently sits in, so the radius is what moves.
        ux, uy = ctab[k]
        r = radius(g, k + phase)
        out.append((int(round(ux * r / 1000.0)), int(round(uy * r / 1000.0))))
    return out


def offsets(gi, frame):
    """The star's points about the centre on the given frame."""
    return offsets_at_phase(gi, phase_of(gi, frame))


def line(a, b):
    """Every pixel on the segment, Bresenham, matching @seg in common.tal."""
    x0, y0 = a
    x1, y1 = b
    dx, dy = abs(x1 - x0), -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    out = []
    while True:
        out.append((x0, y0))
        if x0 == x1 and y0 == y1:
            return out
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def disc(centre, r):
    cx, cy = centre
    return [(x, y)
            for y in range(cy - r, cy + r + 1)
            for x in range(cx - r, cx + r + 1)
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r]


def frame(frame):
    """The lit pixels on a frame, and what each part contributed."""
    lit = set()
    parts = []
    for gi, g in enumerate(GEARS):
        cx, cy = g[0], g[1]
        count = npts(g)
        pts = [(cx + dx, cy + dy) for dx, dy in offsets(gi, frame)]
        own = set()
        for k in range(count):
            own.update(line(pts[k], pts[(k + 1) % count]))
        spoke = set()
        step = max(1, count // SPOKES)
        for k in range(0, count, step):
            spoke.update(line((cx, cy), pts[k]))
        hub = set(disc((cx, cy), HUB))
        parts.append((len(own), len(spoke), len(hub)))
        lit |= own | spoke | hub
    return lit, parts


def moves():
    """Does the gear train actually turn?

    This is the check whose absence let a design through in which every gear
    advanced a whole number of tooth pitches, so every frame was pixel-for-pixel
    identical to every other. The gears meshed perfectly - interlock() was happy
    on all 64 frames - and the demo was a still picture. A constant lit count
    was read as "the loops all ran"; it is equally what a frozen gear produces.

    So: consecutive frames must differ, and the cycle must come all the way round.
    """
    bad = []
    seen = {}
    for f in range(PPTOOTH):
        seen[f] = frozenset(frame(f)[0])
    for f in range(1, PPTOOTH):
        if seen[f] == seen[f - 1]:
            bad.append("frame %d is identical to frame %d: nothing moved"
                       % (f, f - 1))
    if len(set(seen.values())) < PPTOOTH:
        bad.append("only %d distinct pictures in %d frames: a gear's figure "
                   "repeats every whole tooth pitch, so the step has to be "
                   "less than one" % (len(set(seen.values())), PPTOOTH))
    for gi in range(len(GEARS)):
        pts = {f: tuple(offsets(gi, f)) for f in range(PPTOOTH)}
        if len(set(pts.values())) < PPTOOTH:
            bad.append("gear %d never changes shape: its step %+d is a whole "
                       "number of tooth pitches"
                       % (gi + 1, GEARS[gi][5]))
    return bad


def db_tables():
    """The phase tables, as the ROM wants them: all eight phases per gear.

    @star walks a table of offsets and its edge set does not depend on where it
    starts, so a gear cannot be turned by moving the read pointer - the phase
    has to live in the data. One table per phase per gear.
    """
    out = []
    for gi, g in enumerate(GEARS):
        out.append("; gear %d - %d points, %d teeth, centre (%d, %d), root %d, "
                   "tip %d, step %+d"
                   % (gi + 1, npts(g), g[4], g[0], g[1], g[2], g[2] + g[3], g[5]))
        out.append("@tab%d%d" % (gi + 1, gi + 1))
        for ph in range(PPTOOTH):
            row = []
            for k in range(npts(g)):
                dx, dy = offsets_at_phase(gi, ph)[k]
                # A short a coordinate, high half first, because that is the
                # order Dux keeps them in memory: LDZ2 $x reads the high byte
                # at $x. Emitting low-half-first does not fail loudly, it
                # returns 6144 where 24 was meant and draws the gear somewhere
                # else entirely.
                #
                # A short and not a byte, because @widen zero-extends: handed
                # $f9 for -7 it returns 249, not -7. That is not a rounding
                # wobble, it is a gear drawn in the wrong place, and @seg then
                # chases an endpoint it can never reach and the frame never
                # ends. Measured, not assumed - see wip/widenprobe.tal.
                row.append("%d, %d" % ((dx >> 8) & 0xFF, dx & 0xFF))
                row.append("%d, %d" % ((dy >> 8) & 0xFF, dy & 0xFF))
                if len(row) == 8 or k == npts(g) - 1:
                    out.append("        DB " + ", ".join(row))
                    row = []
        out.append("")
    return "\n".join(out)


def check():
    """Everything that has to hold before the count in expect.txt means anything."""
    bad = []
    for gi, g in enumerate(GEARS):
        if g[4] < 1 or PPTOOTH % 2:
            bad.append("gear %d: %d teeth is not a whole number of %d-point "
                       "teeth" % (gi + 1, g[4], PPTOOTH))
        if abs(g[5]) >= PPTOOTH:
            bad.append("gear %d's step %+d is a whole tooth pitch or more, so "
                       "its figure repeats every frame and it does not turn"
                       % (gi + 1, g[5]))
        if g[5] == 0:
            bad.append("gear %d's step is zero" % (gi + 1))
        cx, cy, root, depth = g[0], g[1], g[2], g[3]
        tip = root + depth
        if cx - tip < 0 or cy - tip < 0 or cx + tip >= W or cy + tip >= H:
            bad.append("gear %d at (%d,%d) tip %d leaves the screen"
                       % (gi + 1, cx, cy, tip))
    for a, b in ((0, 1), (1, 2)):
        ga, gb = GEARS[a], GEARS[b]
        if ga[2] * gb[4] != gb[2] * ga[4]:
            bad.append("gears %d,%d: radii %d,%d are not in the tooth ratio "
                       "%d:%d" % (a + 1, b + 1, ga[2], gb[2], ga[4], gb[4]))
        if ga[5] * gb[5] >= 0:
            bad.append("gears %d,%d: rates %+d,%+d turn the same way, so teeth "
                       "that mesh must drive each other"
                       % (a + 1, b + 1, ga[5], gb[5]))
        if abs(ga[5]) != abs(gb[5]):
            bad.append("gears %d,%d: rates %+d,%+d differ in size, so they slip "
                       "at the mesh instead of rolling"
                       % (a + 1, b + 1, ga[5], gb[5]))
        d = math.hypot(gb[0] - ga[0], gb[1] - ga[1])
        want = contact_sum(a, b, 0, PPTOOTH // 2)
        if abs(d - want) > 1.0:
            bad.append("gears %d,%d are %.1f apart but the teeth need %.1f, so "
                       "they either clash or leave a gap"
                       % (a + 1, b + 1, d, want))
    bad.extend(interlock(0))
    bad.extend(moves())
    return bad


def main():
    for b in check():
        print("BAD  " + b)
    n = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 2
    lit, parts = frame(n)
    for gi, (o, s, h) in enumerate(parts):
        print("gear %d: %3d outline, %3d spoke, %3d hub" % (gi + 1, o, s, h))
    print("frame %d: %d lit" % (n, len(lit)))
    if "--tables" in sys.argv:
        print()
        print(db_tables())


if __name__ == "__main__":
    main()