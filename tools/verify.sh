#!/bin/sh
# verify.sh - render every ROM and check it against a recorded prediction.
#
# The expectation file is the point. A pixel count written down *before* the ROM
# was run says how many times each loop ran, which is the fact a rendered
# picture cannot tell you. Checking it in one place also means a ROM cannot ship
# with a prediction nobody wrote.
#
#   ./tools/verify.sh              check everything
#   ./tools/verify.sh cube         check one
#
# Fields are whitespace separated: rom, lit pixels, distinct colours. A count of
# - means "not recorded" and is reported but does not fail.

set -u
cd "$(dirname "$0")/.."

EXPECT=tools/expect.txt
ROMDIR=roms
FRAME=/tmp/dux-verify.ppm
fail=0
ran=0

check_one() {
	rom=$1
	want_px=$2
	want_col=$3
	# Field 4 may be a --bg= override; field 5 may carry DUX_TIME=<epoch> for a
	# ROM that reads live time. Only this invocation is exported, so a pinned
	# clock cannot leak into the other checks.
	env_assign=$(awk -v r="$rom" '$1==r{print $5}' $EXPECT)
	unset DUX_TIME
	case $env_assign in
	DUX_TIME=*) export DUX_TIME=$(echo "$env_assign" | sed 's/.*DUX_TIME=\([0-9]*\).*/\1/') ;;
	esac
	out=$("$ROMDIR/../dux" -n 2 -p "$FRAME" "$ROMDIR/$rom" 2>&1 | grep -v '^dux: ')
	case $out in
	*left*items*)
		echo "FAIL  $rom  stack not balanced:"
		echo "$out" | sed 's/^/        /'
		fail=$((fail + 1))
		return
		;;
	esac
	bg=$(awk -v r="$rom" '$1==r{print $4}' $EXPECT)
	case $bg in
	--bg=*) line=$(python3 tools/ppmcheck.py "$FRAME" "$want_px" "$want_col" "$bg") ;;
	*) line=$(python3 tools/ppmcheck.py "$FRAME" "$want_px" "$want_col") ;;
	esac
	ran=$((ran + 1))
	case $line in
	*FAIL*)
		echo "$line" | sed "s|^|[$rom] |"
		fail=$((fail + 1))
		;;
	*)
		printf '%s\n' "$line" | head -1 | sed "s|^|ok   $rom  |"
		;;
	esac
}

# Report a ROM that has no prediction recorded. Unrecorded is not a failure, but
# it is the state this project spent most of its time in, so it is worth seeing.
unrecorded=0

if [ $# -gt 0 ]; then
	sel=$1
	case $sel in
	*.rom) ;;
	*) sel=$sel.rom ;;
	esac
	check_one "$sel" "$(awk -v r="$sel" '$1==r{print $2}' $EXPECT)" \
		"$(awk -v r="$sel" '$1==r{print $3}' $EXPECT)"
else
	# Every ROM in roms/ that is not a test fixture.
	for f in "$ROMDIR"/*.rom; do
		rom=$(basename "$f")
		case $rom in
		test.rom | sorttest.rom) continue ;;
		esac
		px=$(awk -v r="$rom" '$1==r{print $2}' $EXPECT)
		col=$(awk -v r="$rom" '$1==r{print $3}' $EXPECT)
		if [ -z "$px" ]; then
			echo "FAIL  $rom  no prediction recorded - nobody checked this one"
			fail=$((fail + 1))
			unrecorded=$((unrecorded + 1))
			continue
		fi
		check_one "$rom" "$px" "$col"
	done
fi

echo
if [ "$fail" -gt 0 ]; then
	echo "$fail failed, $ran checked clean"
	exit 1
fi
if [ "$unrecorded" -gt 0 ]; then
	echo "$ran checked, $unrecorded with no prediction recorded"
else
	echo "$ran ROM(s) verified against their predictions"
fi
exit 0