#!/bin/sh
# Assembler regression tests.
#
# Each case assembles one line and compares the emitted bytes. The point is to
# pin down operand parsing, which is where the fiddly rules live: sigils,
# hyphens in names, and the rule that '-' must be space-delimited because a
# name may contain one while no name can contain '*' or '/'.

set -e

ASM=./duxasm
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

# run SOURCE-LINE -> assembles and prints the hex bytes, or fails.
# The rom is removed first and duxasm's status returned, so a failed
# assembly can never be mistaken for the previous case's bytes.
run() {
	{
		echo '@k5         = $05'
		echo '@on-frame   = $07'
		echo '@SCREEN.addr_hi = $09'
		echo '@reset'
		printf '%s\n' "$1"
		echo '        BRK'
	} > "$WORK/case.tal"
	rm -f "$WORK/case.rom"
	$ASM -o "$WORK/case.rom" "$WORK/case.tal" || return 1
	od -An -tx1 -v "$WORK/case.rom" | tr -d ' \n'
}

# check NAME EXPECTED SOURCE
# For the single-LIT forms: "LIT <byte>" is 80 <byte>, so the expectation is
# the operand value alone and this compares just that byte.
check() {
	name=$1
	want=$2
	src=$3

	if ! got=$(run "$src" 2>"$WORK/err"); then
		printf '  FAIL %-30s assemble: %s\n' "$name" "$(head -1 "$WORK/err")"
		fail=$((fail + 1))
		return
	fi

	# Drop the leading LIT opcode (80) and the trailing BRK opcode (00).
	body=${got#80}
	case $body in
		*00) body=${body%00} ;;
	esac

	if [ "$body" = "$want" ]; then
		pass=$((pass + 1))
	else
		printf '  FAIL %-30s want %-8s got %s\n' "$name" "$want" "$body"
		fail=$((fail + 1))
	fi
}

# checkfull NAME EXPECTED-HEX SOURCE
# For instructions that are not a single LIT, compared byte for byte. The
# expectation includes the trailing BRK opcode, 00, that every case ends with.
checkfull() {
	name=$1
	want=$2
	src=$3

	if ! got=$(run "$src" 2>"$WORK/err"); then
		printf '  FAIL %-30s assemble: %s\n' "$name" "$(head -1 "$WORK/err")"
		fail=$((fail + 1))
		return
	fi
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1))
	else
		printf '  FAIL %-30s want %-16s got %s\n' "$name" "$want" "$got"
		fail=$((fail + 1))
	fi
}

# checkerr NAME SOURCE  -- the line must be rejected
checkerr() {
	name=$1
	src=$2
	if run "$src" >/dev/null 2>&1; then
		printf '  FAIL %-30s should have been rejected\n' "$name"
		fail=$((fail + 1))
	else
		pass=$((pass + 1))
	fi
}

echo "assembler:"

echo " literals"
check 'hex literal'       03 '        LIT $03'
check 'decimal literal'   10 '        LIT 16'
check 'binary literal'    05 '        LIT %0101'
check 'two digit hex'     ff '        LIT $ff'
check 'hex ending in a'   2a '        LIT $2a'
check 'hex ending in e'   2e '        LIT $2e'
check 'hex ending in f'   2f '        LIT $2f'
check 'zero'              00 '        LIT $00'

echo " arithmetic"
check 'multiply'          06 '        LIT $3 * 2'
check 'multiply no space' 06 '        LIT $3*2'
check 'add'               2a '        LIT $28 + 2'
check 'add no space'      2a '        LIT $28+2'
check 'subtract'          0f '        LIT $10 - 1'
check 'divide'            10 '        LIT $40 / 4'
check 'shift right'       08 '        LIT $20 >> 2'
check 'shift left'        10 '        LIT $02 << 3'
check 'left to right'     04 '        LIT $1 + 1 * 2'
check 'subtract to neg'   ff '        LIT -1'
check 'negated symbol'    fb '        LIT - &k5'
check 'double negative'   01 '        LIT - -1'
check 'subtract negative' 11 '        LIT $10 - -1'
check 'two operators'     12 '        LIT $10 - 1 + 3'

echo " symbols"
check 'constant'          05 '        LIT $k5'
check 'subtract const'    04 '        LIT $k5 - 1'
check 'hyphenated name'   07 '        LIT $on-frame'
check 'hyphen with op'    06 '        LIT $on-frame - 1'
check 'dotted name'       09 '        LIT $SCREEN.addr_hi'
check 'ampersand name'    05 '        LIT &k5'
check 'hash name'         05 '        LIT #k5'
check 'label ref'         00 '        LIT &reset'
check 'symbol then op'    06 '        LIT &k5 + 1'

echo " sublabels"
# Each scope gets its own &next, so they must resolve to different addresses.
{
	echo '@on-frame'
	echo '        LIT &next'
	echo '        BRK'
	echo '&next'
	echo '        BRK'
	echo '@on-key'
	echo '        LIT &next'
	echo '        BRK'
	echo '&next'
	echo '        BRK'
	echo '@reset'
	echo '        LIT &on-frame'
	echo '        LIT &on-key'
	echo '        BRK'
} > "$WORK/sub.tal"
if $ASM -o "$WORK/sub.rom" "$WORK/sub.tal" 2>"$WORK/err"; then
	# on-frame=0200, on-frame.next=0203, on-key=0204, on-key.next=0207
	if [ "$(od -An -tx1 -v -N12 "$WORK/sub.rom" | tr -d ' \n')" = "800300008007000080008004" ]; then
		pass=$((pass + 1))
	else
		printf '  FAIL %-30s %s\n' 'scoped sublabels' \
			"$(od -An -tx1 -v -N12 "$WORK/sub.rom" | tr -d ' \n')"
		fail=$((fail + 1))
	fi
else
	printf '  FAIL %-30s %s\n' 'scoped sublabels' "$(head -1 "$WORK/err")"
	fail=$((fail + 1))
fi

# A &branch label must not become a scope, so later siblings share the parent.
{
	echo '@routine'
	echo '&one'
	echo '        JMP &shared'
	echo '&two'
	echo '        JMP &shared'
	echo '&shared'
	echo '        BRK'
	echo '@reset'
	echo '        BRK'
} > "$WORK/sub2.tal"
if $ASM -o "$WORK/sub2.rom" "$WORK/sub2.tal" 2>"$WORK/err"; then
	pass=$((pass + 1))
else
	printf '  FAIL %-30s %s\n' 'sibling sublabels' "$(head -1 "$WORK/err")"
	fail=$((fail + 1))
fi

# Two routines may each define &next without colliding.
{
	echo '@alpha'
	echo '&next'
	echo '        BRK'
	echo '@beta'
	echo '&next'
	echo '        BRK'
	echo '@reset'
	echo '        LIT &next'
	echo '        BRK'
} > "$WORK/sub3.tal"
# The reference sits under @reset, so it must not find either routine's next.
if $ASM -o "$WORK/sub3.rom" "$WORK/sub3.tal" 2>"$WORK/err"; then
	printf '  FAIL %-30s should have been rejected\n' 'unscoped reference'
	fail=$((fail + 1))
else
	pass=$((pass + 1))
fi

echo " forms that are not a single LIT"
# STA $k5 $07 -> LIT 05, LIT 07, STA2
checkfull 'STA two operands' '8007a000051500' '        STA $k5 $07'
# DEO takes the port first and the value second, and pushes value then port
checkfull 'DEO port value'   '804180181700' '        DEO $18 $41'
checkfull 'LIT2 wide'        'a0123400' '        LIT2 $1234'
checkfull 'HI high byte'     '801200' '        HI $1234'

echo " rejections"
checkerr 'missing operand'  '        LIT'
checkerr 'bad mnemonic'     '        FLURB $10'
checkerr 'divide by zero'   '        LIT $10 / 0'
checkerr 'operator only'    '        LIT +'
checkerr 'undefined symbol' '        LIT $nope'

echo " data tables"
# A DB line used to stop at sixteen items and return what it had, with no
# message at all. asm/bounce.tal's seventeen-column wordmark lost its last
# column that way and the ROM rendered a "D" with no right-hand edge - 27
# pixels missing, and nothing anywhere said why. Sixteen fitted exactly, so the
# regression starts at seventeen.
#
# The expectations are built rather than written out: a hand-written 36-byte hex
# string is itself a test of the tester's arithmetic, and when it is wrong the
# failure looks like an assembler bug rather than a test bug.
vals=''; i=0
while [ $i -lt 18 ]; do vals="$vals${vals:+, }$i"; i=$((i + 1)); done
dbhex=''; i=0
while [ $i -lt 18 ]; do dbhex="$dbhex$(printf '%02x' $i)"; i=$((i + 1)); done
dwhex=''; i=0
# High byte first, because a short keeps its high byte at the *lower* address -
# so DW $0001 is 00 01 and not 01 00.
while [ $i -lt 18 ]; do dwhex="$dwhex$(printf '00%02x' $i)"; i=$((i + 1)); done
checkfull 'DB eighteen items'  "${dbhex}00" "        DB $vals"
checkfull 'DW eighteen items'  "${dwhex}00" "        DW $vals"
checkfull 'DB space separated' '7f4141413e0000' '        DB $7f $41 $41 $41 $3e $00'
# Past the limit it has to be *refused*. Quietly shortening a table is worse
# than refusing it, because the table comes out the right shape and only its
# tail is missing, which reads as a picture fault and not as an error.
long='        DB'; i=0
while [ $i -lt 70 ]; do long="$long 1"; i=$((i + 1)); done
checkerr 'DB seventy items' "$long"

echo " the stack check"
# A whole program, because the checker works on emitted code rather than on one
# line. Each of these assembles with no warning except the last, which is the
# point of them.
checkprog() {
	name=$1
	body=$2
	want=$3
	printf '@p\n' > "$WORK/p.tal"
	printf '%s\n' "$body" >> "$WORK/p.tal"
	if out=$("$ASM" -o "$WORK/p.rom" "$WORK/p.tal" 2>&1) && [ "$want" = ok ]; then
		pass=$((pass + 1))
	elif [ "$want" = fail ] && printf '%s' "$out" | grep -q stack; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $name"
		printf '  %s\n' "$out"
	fi
}

checkprog 'balanced program' \
'        LIT $01
        STA $10 $00
        JSR &sub
        BRK
@sub
        LDA $10
        JMP2r2' ok

checkprog 'stray push before the break' \
'        LIT $01
        BRK' fail

checkprog 'pop deeper than the stack' \
'        LIT $01
        LIT $02
        ADD2
        BRK' fail

checkprog 'two paths, two depths' \
'        DEI $8c
        LIT $01
        EQU
        JNZ &skip
        LIT $02
&skip
        BRK' fail

checkprog 'a vector is checked too' \
'        LIT2 &vec
        LIT $20
        DEO2
        BRK
@vec
        LIT $01
        BRK' fail

echo " the zero page width lint"
checkzp() {
	name=$1
	body=$2
	want=$3
	printf '@p\n' > "$WORK/z.tal"
	printf '%s\n' "$body" >> "$WORK/z.tal"
	if out=$("$ASM" -o "$WORK/z.rom" "$WORK/z.tal" 2>&1) && [ "$want" = ok ]; then
		pass=$((pass + 1))
	elif [ "$want" = warn ] && printf '%s' "$out" | grep -q 'byte and as a short'; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $name"
		printf '  %s\n' "$out"
	fi
}

checkzp 'one width throughout' \
'@ZP.n = $10
        LDZ2 &ZP.n
        LIT2 $0001
        ADD2
        STA2 &ZP.n
        BRK' ok

# DW emits every comma-separated field, exactly as DB does. Taking only the
# first is the same as a DB that quietly dropped the rest: the table comes out
# the right shape and full of zeros, and nothing complains.
checkfull 'DW one field' '0f4f00' 'DW $0f4f'

checkfull 'DW three fields' '0f4f0abc011c00' 'DW $0f4f, $0abc, $011c'

# The stride matters more than the bytes: emitting only the first field gives
# the right shape and a table of zeros, and nothing else notices.
checkfull 'DW table stride' \
'003f000600' 'DW $003f, $0006'

# A two-operand store takes its operands as address then value, the order they
# are written in, and swaps them on the way out so the value lands on top for
# the core. These pin that down, because reading it the other way round stores
# the address *as* the value: a program writing $beef to $1000 puts $1000 at
# $beef instead, which is past the framebuffer and shows up as corruption a long
# way from the mistake.
checkfull 'STA2 address then value' \
'a0beefa010003500' 'STA2 $1000 $beef'

checkfull 'STA2 zeroes a byte pair' \
'a00000a000103500' 'STA2 $0010 $0000'

# One operand is a zero page index, the other a value, and the order still holds.
checkfull 'STA2 into the zero page' \
'a0beefa000103500' 'STA2 $10 $beef'

# The short forms of the comparisons, which for a long time had no coverage
# anywhere: a program comparing shorts with the byte opcode silently compares
# the low bytes and leaves a byte on the stack each time.
checkfull 'EQU2' 'a00f4fa00f4f2800' 'LIT2 $0f4f
        LIT2 $0f4f
        EQU2'

checkfull 'GTH2' 'a00005a000032a00' 'LIT2 $0005
        LIT2 $0003
        GTH2'

checkzp 'a byte store into a short' \
'@ZP.n = $10
        STZ &ZP.n $00
        LDZ2 &ZP.n
        STZ2 &ZP.n
        BRK' warn

echo
if [ "$fail" -eq 0 ]; then
	echo "$pass assembler checks, 0 failures"
else
	echo "$pass assembler checks, $fail failures"
	exit 1
fi