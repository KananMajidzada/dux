#!/usr/bin/env python3
"""Report the net stack change of every routine in a .tal file.

    netcheck.py file.tal

The assembler has a stack checker and it is the oracle - but it reports a
*total* at the break, and on a file this size "6 bytes" does not say which of
six routines leaked them, let alone which of the four inside one of them. This
walks the same lines and prints the arithmetic per routine, so the leak gets
named instead of hunted for.

Three things this has to get right, and each of them was wrong in an earlier
version of this file, which is why they are worth writing down:

  * A label is a routine boundary only when something calls it. Internal labels
    - @sg-loop and its eleven siblings - also sit in column one, and treating
    every label as a boundary splits a routine into fragments that each look
    unbalanced and say nothing about any of them.

  * A routine's change is the depth at its **exits** minus the depth at entry,
    and the exits are per path. Summing a whole body counts every branch, so a
    routine with two JMP2r2 exits comes out non-zero while every path through it
    is balanced. That is @advance, and it read as a two-byte leak for a while.
    So this propagates a depth through the intra-routine control flow to a
    fixpoint and reports what it finds at each exit.

  * A non-zero net is **not** a bug. The assembler's own checker does not
    require one: it reports an address reached at two different depths, a pop
    from a stack that is not that deep, and leftovers at a break. A routine
    that pushes a result or takes an argument is fine, which is why @pair in
    clock.tal - a verified ROM - is +1 and never mentions it again. So calling
    every positive net a leak is wrong, and it was: it flagged thirteen
    routines in pong, five of them in a ROM the assembler passes.

    What *is* a bug is a routine whose exits disagree with each other, because
    then a caller reaches the return point at two different depths depending on
    which path it took. That is checked here, and the net is reported beside it
    because it is what you add up by hand when a caller and a callee disagree.

  * A routine called from an INCLUDE has to be *measured*, not assumed to be
    zero. @widen is net +1 and @plot net -5; calling either of them zero makes
    every caller look short and buries the real disagreement under false ones.

The net changes are the ones asm/stackcheck.c models, each pseudo-op counting
the LIT it implies:

    LIT +1     LDA2 $nn +2   LDA2 (bare) 0     STA2 $nn -2
    LIT2 +2    LDA  $nn +1   LDA  (bare) -1    STA  $nn -1
    LDZ  +1    LDZ2 +2       STA (bare) -3     STA2 (bare) -2
    ADD/SUB/MUL/DIV -1        ADD2/SUB2/MUL2/DIV2 -2
    GTH/LTH/EQU/NEQ -1        GTH2/LTH2/EQU2/NEQ2 -3
    JNZ -1      JMP 0         JSR2 0            JMP2r2 0

DUP is +1 for a byte, and a defaulted store (`STA2 $46 $0000`, which the
assembler pushes a default for) is net zero. Both are in the table below
because their absence made every store in every file read as a no-op.
"""

import os
import re
import sys

NET = {
    "LIT": 1, "LDA": 1, "LDZ": 1, "DUP": 1,
    "LIT2": 2, "LDA2": 2, "LDZ2": 2, "OVR": 0, "ROT": 0, "SWP": 0, "SFT": 0,
    "ADD": -1, "SUB": -1, "MUL": -1, "DIV": -1,
    "AND": -1, "ORA": -1, "EOR": -1,
    "ADD2": -2, "SUB2": -2, "MUL2": -2, "DIV2": -2,
    "AND2": -2, "ORA2": -2, "EOR2": -2,
    "GTH": -1, "LTH": -1, "EQU": -1, "NEQ": -1,
    "GTH2": -3, "LTH2": -3, "EQU2": -3, "NEQ2": -3,
    # a store with an operand is LIT2 address then the opcode, which pops the
    # address and the value: -1 for a byte, -2 for a short. Without the operand
    # the address comes off the stack instead, and it is in BARE below.
    "STA": -1, "STA2": -2, "STZ": -1,
    "JNZ": -1, "JMP": 0, "JMP2": 0, "JSR2": 0, "JMP2r2": 0, "JMP2r": 0,
    # SFT takes its control byte off the stack, which is why nothing is emitted
    # in front of it; POP moves the return stack, not the write stack.
    "SFT": 0, "SFT2": 0, "POP": 0, "POP2": 0,
    # DEO's port is one byte whatever the mode, so the operand is always a LIT
    # and the opcode pops 1 + w: -1 for a byte value, -2 for a short one.
    "DEO": -1, "DEO2": -2, "DEI": 1,
}
BARE = {"LDA": -1, "LDA2": 0, "STA": -3, "STA2": -2,
        # with no operand there is no LIT in front, so the port comes straight
        # off the stack: DEO2 pops the port and a short, three bytes, not two
        "DEO": -2, "DEO2": -3, "DEI": 0}
# A store may carry a default value - `STA2 $46 $0000` - which the assembler
# pushes itself. The emitted bytes are LIT2 default, LIT2 address, opcode, so
# the whole line is net zero rather than -2.
DEFAULTED = ("STA", "STA2", "STZ")
# @name, $name and the &name local labels all get jumped to.
SYMBOL = re.compile(r"^[@$&][A-Za-z0-9_.\-]+")


def read(path):
    out = []
    for line in open(path):
        line = line.split(";")[0].strip()
        if line:
            out.append(line)
    return out


def split(lines, called):
    """The label map and the called routines' entry points, in file order."""
    labels, entries = {}, []
    for i, ln in enumerate(lines):
        m = SYMBOL.match(ln + " ")
        if m:
            labels[m.group(0)] = i
            if m.group(0) in called:
                entries.append((m.group(0), i))
    return labels, entries


def analyse(lines, labels, start, end, net):
    """Walk from start to end, returning the depth found at each exit.

    A jump is a branch, not arithmetic: `JMP @x`, the short `JMP2 @x` and
    `JNZ @x` all stop the fall-through. Missing JMP2 here is what made pong -
    a verified ROM the assembler itself passes - read as leaking in thirteen
    places, because each JMP2 was counted as a no-op and the lines after it as
    unreachable.
    """
    BRANCH = ("JMP", "JMP2", "JNZ")
    exits = []
    depth = {start: 0}
    work = [start]
    while work:
        i = work.pop()
        d = depth.get(i)
        if d is None:
            continue
        j = i
        while j < end:
            ln = lines[j]
            j += 1
            parts = ln.split(None, 1)
            op = parts[0]
            arg = parts[1].strip() if len(parts) > 1 else ""
            if op in ("JMP2r2", "JMP2r") or \
                    (op in ("JMP", "JMP2") and not arg):
                exits.append((ln, d))
                break
            if op == "JSR2":
                # a call, not an exit: the callee's change is folded in and
                # the walk carries on past it
                d += net.get(arg, 0)
                continue
            if op in DEFAULTED and len(arg.split()) > 1:
                d += 0
                continue
            d += BARE[op] if (not arg and op in BARE) else NET.get(op, 0)
            if op in BRANCH and arg:
                m = SYMBOL.search(arg)
                t = m.group(0) if m else None
                if t is None or t not in labels:
                    break
                k = labels[t]
                if depth.get(k) is None:
                    depth[k] = d
                    work.append(k)
                if op in ("JMP", "JMP2"):
                    break
            else:
                while j < end and SYMBOL.match(lines[j] + " "):
                    j += 1
                if j < end and depth.get(j) is None:
                    depth[j] = d
                    work.append(j)
    return exits


def measure(path, wanted, depth_guard=0):
    """Net change of each label in `wanted`, following INCLUDE.

    The includes are measured *first* and their results seed the fixpoint.
    Measuring them afterwards leaves @put16 at zero while the arithmetic that
    needs it is being summed, and every routine that calls it comes out short
    by three - which is most of a file that draws anything.
    """
    if not os.path.exists(path):
        return {}
    lines = read(path)
    called = set()
    for ln in lines:
        if ln.startswith("JSR2 "):
            called.add(ln.split()[1].strip())
    called |= set(wanted)

    # 1. the includes, first
    net = {}
    if depth_guard < 4:
        for ln in lines:
            if ln.upper().startswith("INCLUDE "):
                inc = ln.split(None, 1)[1].strip().strip('"')
                p2 = os.path.join(os.path.dirname(path) or ".", inc)
                for k, v in measure(p2, wanted, depth_guard + 1).items():
                    net.setdefault(k, v)

    # 2. this file's own routines, to a fixpoint
    labels, entries = split(lines, called)
    starts = [at for _, at in entries]
    bounds = {}
    for k, (name, at) in enumerate(entries):
        end = starts[k + 1] if k + 1 < len(starts) else len(lines)
        bounds[name] = (at, end)
    for name, _at in entries:
        net.setdefault(name, 0)

    for _ in range(len(entries) + 3):
        changed = False
        for name, _at in entries:
            at, end = bounds[name]
            exits = analyse(lines, labels, at, end, net)
            n = exits[0][1] if exits else 0
            if net[name] != n:
                net[name], changed = n, True
        if not changed:
            break

    return {n: net[n] for n in wanted if n in net}


def main():
    path = sys.argv[1]
    lines = read(path)
    called = set()
    for ln in lines:
        if ln.startswith("JSR2 "):
            called.add(ln.split()[1].strip())
    called.add("@draw")

    net = measure(path, sorted(called))
    labels, entries = split(lines, called)

    print("%-12s %5s  %s" % ("routine", "net", "exits"))
    bad = 0
    for name, at in entries:
        end = len(lines)
        for k, (n2, a2) in enumerate(entries):
            if a2 > at:
                end = a2
                break
        if not any(lines[j].strip() for j in range(at, end)):
            print("%-12s %5s  %s" % (name, "?", "called but defined elsewhere"))
            continue
        netv = net.get(name, 0)
        exits = analyse(lines, labels, at, end, net)
        depths = sorted({d for _ln, d in exits})
        where = ", ".join(str(d) for d in depths) if depths else "none found"
        if len(depths) > 1:
            note = "EXITS DISAGREE at %s  <-- a caller reaches these at two depths" % where
            bad += 1
        elif not depths:
            note = "no exit found - falls off the end?"
            bad += 1
        else:
            sign = "balanced" if netv == 0 else (
                "pushes %d, callers must account" % netv if netv > 0
                else "consumes %d, callers supply it" % -netv)
            note = "return depth %s  (%s)" % (where, sign)
        print("%-12s %5d  %s" % (name, netv, note))
    print()
    print("%d routine(s) with exits that disagree" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
