#!/usr/bin/env python3
"""Predict a ROM's net stack depth, statically.
import sys

    tools/stacksim.py asm/cube.tal

`duxasm` links a stack checker and is silent on both ROMs on this project that
actually leak, so a clean assemble means only that it parsed. This is the
alternative: read the source and add up.

**A negative number here does not mean the ROM leaks.** This tool counts *call
sites*, not calls, so a loop body is charged once however many times it runs. A
ROM whose inner loop contains an arithmetic op therefore reads negative by
exactly one body's worth - and every one of the thirty-odd ROMs here does,
while all of them are balanced in fact. Only a *non-zero* result means something,
and only when the caller has already been accounted for.

That is why `tools/routinegraph.py` exists and why it is the one to believe: it
walks the call graph, follows conditional branches and cuts back edges, and
reads zero on all thirty-four ROMs, including the two that leak.

The widths below are measured against the VM by `tools/probe.sh`. Three of them
were wrong for most of this project's life and no ROM could show it, because
every site using them paired a load with the operation and both the right and the
wrong value read as zero: the binary short ops take two shorts and return one
(-4, not -2), and the bare forms of `STA`/`STA2` also take a two-byte address off
the stack. A short operation takes two shorts and returns one, so it is -2, and
`ADD2 = -4` was a misreading of the probe: four bytes in and two left is a net
of **-2**, not -4. **A width that only ever appears beside its own negation cannot
be calibrated by agreement**, which is how that mistake survived a reading.
"""

import sys

# Widths measured against the VM with tools/probe.sh, not inferred.
W2 = {'LDA2':+2,'LDZ2':+2,'LDA2R':+2,'STA2':-2,'LIT2':+2,
      'ADD2':-2,'MUL2':-2,'SUB2':-2,'DIV2':-2,
      'AND2':-2,'ORA2':-2,'EOR2':-2,
      'EQU2':-3,'GTH2':-3,'LTH2':-3,'NEQ2':-3,'SFT2':-1,'LDA':-1}
W1 = {'LDZ':+1,'STA':-1,'LIT':+1,'ADD':-1,'AND':-1,'ORA':-1,'EOR':-1,
      'SUB':-1,'MUL':-1,'DIV':-1,'EQU':-1,'NEQ':-1,'GTH':-1,'LTH':-1,'SFT':-1,
      # Device access. With an inline port the port is not on the stack: DEI
      # pushes the byte it read and DEO pops the byte it writes, so a
      # `LIT $xx / DEO $port` pair is balanced. Omitting these (they default to
      # 0) happened to cancel against the paired LIT at every balanced site,
      # which is why 121 uses across the project went unnoticed.
      'DEI':+1,'DEO':-1}
CALLEE = {'@wide':+1,'@widen':+1,'@seti2':-1,'@getsx':-2,'@getsy':-2,
          '@facecolour':0,'@abs2':0,'@bigger':0,'@small':0,'@negate':0,
          '@plot':-5,'@clear':0,'@toflag':-1,'@plotat':-1,'@show':-3,
          '@put16':-3,'@flag2':+1,'@div2':0,'@turn':0,'@project':0,
          '@face-depths':0,'@sort':0,'@edgey':0,'@line':0,'@term':0,
          '@smul':0,'@shr6':0,'@cos':+1,'@sin':+2,'@frame-body':0,'@fill':0,'@on-frame':0,'@takei':-1}

def simulate(path):
    depth, unknown = 0, set()
    for raw in open(path):
        s = raw.strip()
        if not s or s.startswith(';') or s.startswith('INCLUDE'):
            continue
        parts = s.split()
        op = parts[0]
        arg = parts[1] if len(parts) > 1 else ''
        if op in ('STA', 'STA2') and len(parts) == 1:
            depth += -3 if op == 'STA' else -4   # address also comes off the stack
        elif op in ('LDA2', 'LDA') and len(parts) == 1:
            depth += 0 if op == 'LDA2' else -1  # bare form pops an address too
        elif op in ('STA', 'STA2') and len(parts) > 2:
            pass        # immediate form: pushes the value and pops it, net zero
        elif op == 'JSR2':
            if arg not in CALLEE:
                unknown.add(arg)
            depth += CALLEE.get(arg, 0)
        elif op == 'JNZ':
            depth -= 1              # supplies its own two-byte address
        elif op in ('JMP','JMP2','JCI','JMI','JMP2r2','BRK'):
            pass
        else:
            depth += W2.get(op, W1.get(op, 0))
    return depth, unknown

if __name__ == '__main__':
    for p in sys.argv[1:]:
        d, u = simulate(p)
        print('%-24s %+d%s' % (p, d, '  unmeasured callees: %s' % sorted(u) if u else ''))

