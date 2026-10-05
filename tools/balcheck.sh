#!/bin/sh
# balcheck.sh - every ROM must end every frame with an empty working stack.
#
#   ./tools/balcheck.sh          all ROMs in roms/
#   ./tools/balcheck.sh cube     one ROM
#
# Why this is separate from verify.sh: verify.sh asks whether a ROM draws what
# was predicted, which is a question about pixels. This asks whether it hands the
# host back the stack it was given, which is a question about the machine.
#
# The host already refuses an unbalanced vector, but it latches: the first
# failure stops the report, so a ROM that leaked 6 bytes a frame reported "left
# 6 items" once and then said nothing for the next fifteen frames. That is enough
# to notice a bug and not enough to characterise one. DUX_REPORT_DEPTH reports
# the depth after *every* frame instead, which says not just that a ROM leaks but
# how much it leaks per frame - and the rate is what identifies the fault. A
# steady 6 meant one byte per sort pass; a fixed 12 meant something at reset.
#
# So this checks two things per ROM, and both have caught real bugs the pixel
# count could not: that no frame ends non-zero, and that the depth does not
# drift. test.rom installs no frame vector, so it has nothing to report and is
# skipped rather than counted as a pass.

set -u
cd "$(dirname "$0")/.."

DUX=./dux
FRAMES=8
fail=0
ran=0

if [ $# -gt 0 ]; then
	sel=$1
	case $sel in
	*.rom) ;;
	*) sel=$sel.rom ;;
	esac
	list="roms/$sel"
else
	list=""
	for f in roms/*.rom; do list="$list $f"; done
fi

for f in $list; do
	[ -f "$f" ] || continue
	rom=$(basename "$f")
	case $rom in
	# The self test: it runs its checks at reset and installs no frame vector,
	# so there is no per-frame depth to watch.
	test.rom | sorttest.rom) continue ;;
	esac

	depths=$(DUX_REPORT_DEPTH=1 "$DUX" -n "$FRAMES" "$f" 2>&1 \
		| grep -oE 'vector 20: [0-9]+' | awk '{print $3}')
	if [ -z "$depths" ]; then
		echo "skip  $rom  no screen vector installed"
		continue
	fi

	bad=$(echo "$depths" | grep -c '[^0]')
	distinct=$(echo "$depths" | sort -u | tr '\n' ' ')
	ran=$((ran + 1))

	if [ "$bad" -ne 0 ]; then
		first=$(echo "$depths" | head -1)
		echo "FAIL  $rom  $bad of $FRAMES frames ended non-empty: depths $distinct"
		fail=$((fail + 1))
		continue
	fi
	if [ "$(echo "$depths" | sort -u | wc -l)" -ne 1 ]; then
		echo "FAIL  $rom  depth drifted across frames: $distinct"
		fail=$((fail + 1))
		continue
	fi
	echo "ok    $rom  depth 0 after all $FRAMES frames"
done

echo
if [ "$fail" -gt 0 ]; then
	echo "$fail failed, $ran checked"
	exit 1
fi
echo "$ran ROM(s) returned an empty stack after every frame"
exit 0