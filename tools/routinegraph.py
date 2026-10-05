#!/usr/bin/env python3
"""Compute the caller-visible stack net of a routine by walking the call graph.

Why this exists
---------------
The first attempt measured a routine by splitting the source on `^@` and
simulating the block in isolation.  That is wrong: labels are not routines.
`@colon`, `@draw-one` and `@erase-one` in `clock.tal` are branch *targets
inside* their callers, so a split on `^@` fragments a routine at its own
internal labels and the "per-routine" numbers are per-label-block.  Applying
them made the prediction worse (50 -> 22).

A routine is what is reachable from a call site: every basic block reached by
following intra-routine branches, stopping at the `JMP2r2` that returns.  That
is a graph property, so this walks it.

A width depends on an opcode's *form*, not its name.  Everything learned by
calibration lives in the tables below; see NOTES.md for how each was measured.
"""

import re
import sys as _sys
_sys.setrecursionlimit(100000)
import sys

# ---------------------------------------------------------------- widths

# Short (two-byte) operations.  Value is the net change in stack bytes.
W2 = {
    'LDA2': +2,   # LDA2 $nn pushes a short; bare LDA2 pops a short address and
                 # pushes a short, so the two forms differ by two bytes.
    'LDZ2': +2,
    'LIT2': +2,
    'STA2': -2,   # STA2 $nn: the address is inline, the short comes off the stack
    'ADD2': -2, 'SUB2': -2, 'MUL2': -2, 'DIV2': -2,
    'AND2': -2, 'ORA2': -2, 'EOR2': -2,
    # Short comparisons return one byte of flag, and their operands.
    'EQU2': -3, 'GTH2': -3, 'LTH2': -3, 'NEQ2': -3,
    'SFT2': -1,   # byte control, short value, value pushed first
}

# Byte operations: pop two, push one.
W1 = {
    'LDZ': +1, 'LIT': +1,      # push one
    'STA': -1,                 # STA $nn: the address is inline, the byte is not
    'ADD': -1, 'SUB': -1, 'MUL': -1, 'DIV': -1,
    'AND': -1, 'ORA': -1, 'EOR': -1,
    'EQU': -1, 'NEQ': -1, 'GTH': -1, 'LTH': -1,
    'SFT': -1,
}

# The bare forms of the absolute-address ops, measured against the VM rather
# than inferred: dux.c reads `OPC(STA, PO2(a) GET(y), ...)`, and PO2 always
# takes two bytes for the address whatever the value's width. So a bare STA
# consumes a two-byte address and a byte, and a bare STA2 a two-byte address and
# a short. Getting these wrong by two bytes each is what made @turn look like
# the worst offender on cube when it is in fact slightly negative.
#
# Measured with tools/probe.sh: `LIT2 $addr / LIT $val / STA` leaves nothing,
# and so does `LIT2 $val / LIT2 $addr / STA2`. The two-operand forms push their
# own address literal and are net zero, and `STA $nn` / `STA2 $nn` inline only
# the address.
W_BARE = {'STA': -3, 'STA2': -4}

# Device access.  An inline port means the port is not on the stack; the bare
# form takes the port off the stack as well.
IO = {
    'DEI':  +1,   # pushes the byte read
    'DEO':  -1,   # pops the byte written
    'DEI2': +1,   # bare: pops port, pushes a short -> -3 + 2
    'DEO2': -3,   # bare: pops value (2) and port (1)
}

# Stack-neutral control transfer.
ZERO = {'JMP', 'JMP2', 'JMP2r2', 'BRK', 'INC', 'SWP', 'ROT', 'OVR'}
# Pops the one-byte flag that a comparison left.
POPFLAG = {'JNZ', 'JCI', 'JMI'}
# Falls through to whatever follows.
FALLTHRU = {'POP': -1, 'NIP': -2, 'DUP': +1}

BRANCH_OPS = {'JNZ', 'JCI', 'JMI', 'JMP', 'JMP2'}
# Only JMP/JMP2 are unconditional.  A conditional falls through when its flag is
# false, and skipping that path misses half of every dispatch chain - which is
# exactly what @draw-one is built out of.
UNCONDITIONAL = {'JMP', 'JMP2'}
TERMINATORS = {'JMP2r2', 'BRK'}


def width(parts):
    """Net stack change of one instruction.  None if it cannot be decided."""
    op = parts[0]
    n = len(parts)
    if op in ZERO:
        return 0
    if op in POPFLAG:
        return -1
    if op in FALLTHRU:
        return FALLTHRU[op]
    if op in BRANCH_OPS or op in TERMINATORS:
        return 0
    if op == 'JSR2':
        return None                       # resolved by the caller
    # Bare LDA2 pops a short address; the two-byte form does not.
    if op == 'LDA2' and n == 1:
        return 0
    if op == 'LDA' and n == 1:
        return -1
    if op in W_BARE and n == 1:
        return W_BARE[op]                 # address comes off the stack too
    if op in ('STA', 'STA2') and n > 2:
        return 0                          # STA $nn $00 also supplies its address
    if op == 'DEO2' and n == 1:
        return IO['DEO2']
    if op == 'DEI2' and n == 1:
        return IO['DEI2']
    if op in W2:
        return W2[op]
    if op in W1:
        return W1[op]
    if op in IO:
        return IO[op]
    return 0                              # data directives and unknowns


# ---------------------------------------------------------------- parsing

LABEL = re.compile(r'^(@[A-Za-z0-9_.\-]+|&[A-Za-z0-9_.\-]+)\s*$')


def parse(path):
    """Return (instructions, labels) where instructions is a list of
    (label_or_None, tokens) and labels maps a name to its instruction index."""
    instrs = []
    labels = {}
    pending = []
    for raw in open(path):
        s = raw.split(';')[0].strip()
        if not s:
            continue
        if s.upper().startswith('INCLUDE'):
            continue
        m = LABEL.match(s)
        if m:
            pending.append(m.group(1))
            continue
        idx = len(instrs)
        for name in pending:
            labels[name] = idx
        pending = []
        instrs.append((None, s.split()))
    for name in pending:
        labels.setdefault(name, len(instrs))
    return instrs, labels


# ---------------------------------------------------------------- the walk

class Net:
    def __init__(self, path, verbose=False):
        self.instrs, self.labels = parse(path)
        self.path = path
        self.cache = {}
        self.active = set()
        self.unknown = set()
        self.verbose = verbose
        self.ambiguous = set()   # routines whose net depends on the path

    def block(self, label):
        """Every distinct net from `label` to a return.

        This has to be a path sum, not a sum over reachable instructions. A
        routine with two arms holds each instruction once, so adding them all
        counts both arms even though exactly one runs: `@abs2` is +0 down
        either path and came out as +2 by having both arms added together.
        Summing the union is only valid for straight-line code.

        Returns a set of nets. One value means the routine is balanced the same
        way however it is entered. Several means the contract is a function of
        the path, which is worth seeing rather than averaging away.

        A back edge is cut rather than followed, so a loop body is charged once
        and the search is over acyclic paths. Cutting has to be decided *first*:
        relaxing the graph while cycles are still in it does not converge,
        because a loop body with a net cost feeds itself, and the first version
        of this returned nonsense for exactly that reason.

        So: one depth-first pass marks every edge that closes a loop and records
        the finish order, then the bounds are computed in reverse finish order,
        which is a topological order of what is left. Each instruction gets a
        smallest and a largest net still to be had, and the routine's answer is
        the pair at its returns. Callers use the minimum, so a leak is not
        inflated by an arm that happens to give bytes back.
        """
        start = self.labels[label]
        back = set()
        state = {}                        # 0 = on the stack, 1 = finished
        post = []

        def dfs(i):
            state[i] = 0
            for t, _w in self.edges(i):
                if state.get(t) == 0:
                    back.add((i, t))      # closes a loop
                elif t not in state:
                    dfs(t)
            state[i] = 1
            post.append(i)

        dfs(start)

        lo, hi = {}, {}
        for i in reversed(post):
            if i >= len(self.instrs) or self.instrs[i][1][0] in TERMINATORS:
                lo[i] = hi[i] = 0
                continue
            if self.instrs[i][1][0] == 'JSR2':
                c = min(self.nets_of(self.instrs[i][1][1]))
                lo[i] = c + lo.get(i + 1, 0)
                hi[i] = c + hi.get(i + 1, 0)
                continue
            w = width(self.instrs[i][1])
            outs = [t for t, _ in self.edges(i) if (i, t) not in back and t in lo]
            if outs:
                lo[i] = min(lo[t] for t in outs) + w
                hi[i] = max(hi[t] for t in outs) + w
            else:
                lo[i] = hi[i] = 0          # fell off the end: a return anyway
        nets = set()
        for i in post:
            if i < len(self.instrs) and self.instrs[i][1][0] in TERMINATORS:
                nets.add(lo[i])
                nets.add(hi[i])
        return nets

    def edges(self, i):
        """Successors of instruction `i` with the net cost of getting there."""
        if i >= len(self.instrs):
            return []
        _, parts = self.instrs[i]
        op = parts[0]
        if op in TERMINATORS:
            return []
        if op == 'JSR2':
            # A call is charged its callee's smallest possible net, and returns
            # to the next instruction. Taking the minimum keeps the caller
            # looking only for leaks, which are the thing worth bounding.
            return [(i + 1, min(self.nets_of(parts[1])))]
        w = width(parts)
        if op in BRANCH_OPS:
            out = []
            if len(parts) > 1 and parts[1] in self.labels:
                out.append((self.labels[parts[1]], w))
            if op not in UNCONDITIONAL:
                out.append((i + 1, w))
            return out
        return [(i + 1, w)]

    def nets_of(self, label):
        """The set of distinct nets of the routine entered at `label`."""
        if label in self.cache:
            return self.cache[label]
        if label not in self.labels:
            self.unknown.add(label)
            return {0}
        if label in self.active:
            return {0}                       # recursion: charge nothing
        self.active.add(label)
        nets = self.block(label)
        self.active.discard(label)
        if len(nets) > 1:
            self.ambiguous.add(label)
        self.cache[label] = nets
        return nets

    def net_of(self, label):
        """A single representative net: 0 if balanced, else the first value."""
        nets = self.nets_of(label)
        return 0 if nets == {0} else sorted(nets)[0]

    def net_of(self, label):
        """Caller-visible net of the routine entered at `label`."""
        if label in self.cache:
            return self.cache[label]
        if label not in self.labels:
            self.unknown.add(label)
            return 0
        if label in self.active:
            # A cycle: a routine that can reach itself.  Charge nothing rather
            # than recurse forever, and say so.
            return 0
        self.active.add(label)
        net = self.block(label)
        self.active.discard(label)
        self.cache[label] = net
        return net

    def run(self, entry):
        return self.net_of(entry), self.unknown


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    net = Net(args[0])
    for label in args[1:] or ['@reset']:
        nets = net.nets_of(label)
        show = '{%s}' % ', '.join('%+d' % n for n in sorted(nets))
        extra = ''
        if label in net.ambiguous:
            extra += '  PATH-DEPENDENT'
        if net.unknown:
            extra += '  unmeasured: %s' % sorted(net.unknown)
        print('%-18s %s%s' % (label, show, extra))
    return 0


if __name__ == '__main__':
    sys.exit(main())