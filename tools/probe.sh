#!/bin/sh
# probe.sh - run the ISA probes and check they still report what they were
# written to establish.
#
#   ./tools/probe.sh
#
# These are not ROMs: they draw nothing and print to the console, because what
# they are for is answering a question about the machine and being read. They
# earn their place by the output being checkable, so the expected lines live here
# next to the tool that runs them.
#
# Every finding below was established by measurement rather than by reading the
# C, and three of them contradicted the C. That is the whole value of keeping
# them: a future change to an opcode that quietly inverts a comparison would
# show up here as a changed line rather than as a subtly wrong picture.

set -u
cd "$(dirname "$0")/.."

# INCLUDE resolves next to the file that asks for it, so the prelude has to be
# reachable from wip/ for these to assemble at all.
cp asm/common.tal wip/common.tal
trap 'rm -f wip/common.tal' EXIT

fail=0
check() {
	name=$1
	want=$2
	got=$(./duxasm -o "/tmp/probe-$name.rom" "wip/$name.tal" >/dev/null 2>&1 \
		&& ./dux -n 1 "/tmp/probe-$name.rom" 2>/dev/null | tr -d '\n')
	if [ "$got" = "$want" ]; then
		echo "ok    $name  $got"
	else
		echo "FAIL  $name"
		echo "        want $want"
		echo "        got  $got"
		fail=$((fail + 1))
	fi
}

# probe12: the comparison directions. GTH2 tests the first value pushed against
# the second, so with si=10, sj=5 the pair is (1,0) and with si=5, sj=10 it is
# (0,1). The last four numbers are SUB2 on 160 and 5.
check probe12 '1,0,0,1,155,165,1,0,0,1,155,165,'

# probe14: a comparison whose left operand came out of AND2. The flag follows the
# masked value, and 10 & 15 = 10, which is above 7 but below 0x0f.
check probe14 '0,1,1,0,10,0,1,1,0,10,'

# probeA and probeB: four nested byte loops bounded at 10, 10, 8 and 8. Both read
# the same because probeB differs only in what it does after the loops.
check probeA '10,10,8,8,10,10,8,8,'
check probeB '10,10,8,8,10,10,8,8,'

echo
if [ "$fail" -gt 0 ]; then
	echo "$fail probe(s) no longer report what they were written to show"
	exit 1
fi
echo "4 probes report their expected findings"
exit 0
