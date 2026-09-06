#!/bin/sh
#
# Experiment 012 instrument self-test. NO SOUND IS EMITTED.
#
# WHY THIS EXISTS
#
# The band chosen by this experiment goes into a profile, and a profile is
# hard to walk back. The selection therefore has to be wrong-proof against the
# instrument itself before it is pointed at a room: a minimax that quietly
# maximised the average, or that failed to notice a notch, would produce a
# confident number with nothing behind it.
#
# Every case below has an answer that is known without measuring anything.
#
#   ./selftest.sh          uses cc
#   CC=clang ./selftest.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-cc}
WORK=${TMPDIR:-/tmp}/mcl-exp012-selftest.$$
BS="$WORK/band_sweep"
FAIL=0

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

$CC -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 \
    -o "$BS" "$HERE/band_sweep.c" -lm

ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf '  FAIL %s\n' "$1"; FAIL=$((FAIL + 1)); }

# A synthetic path. Every tone at `flat` dBFS, except those named in $4 which
# are put at `notch` -- a loudspeaker notch, which is the thing that makes this
# experiment necessary rather than a formality.
# The room floor is a flat -90 dBFS in every fixture, so SNR is simply the
# level plus 90. Selection is scored on SNR: see level_at() in band_sweep.c.
mkpath() {
    out=$1; label=$2; flat=$3; notch=$4; notched=$5
    {
        echo "# band_sweep slots"
        echo "# label	$label"
        printf '# capgain\t0.120000\n'
        printf '# emitgain\t50.0\n'
        echo "hz	own_db	leak_db	floor_db	snr_db	own_vs_leak_db"
        f=1200
        while [ "$f" -le 9000 ]; do
            lvl=$flat
            for n in $notched; do
                [ "$n" = "$f" ] && lvl=$notch
            done
            snr=$((lvl + 90))
            printf '%s\t%s.00\t-95.00\t-90.00\t%s.00\t5.00\n' "$f" "$lvl" "$snr"
            f=$((f + 300))
        done
    } > "$out"
}

echo "=== experiment 012 instrument self-test ==="
echo "no sound is emitted by this script"
echo

# ---------------------------------------------------------------- case 1
# One path, flat. Every admissible pair scores the same, so the only wrong
# answer is an inadmissible one: f0 below 1500, or a separation under 900 Hz.
mkpath "$WORK/flat.tsv" flat -20 -20 ""
out=$("$BS" minimax "$WORK/flat.tsv")
f0=$(echo "$out" | sed -n 's/^WINNER  \([0-9]*\) .*/\1/p')
f1=$(echo "$out" | sed -n 's/^WINNER  [0-9]* \/ \([0-9]*\) .*/\1/p')
if [ "$f0" -ge 1500 ] && [ $((f1 - f0)) -ge 900 ] && [ "$f1" -le 9000 ]; then
    ok "flat path yields an admissible pair ($f0/$f1)"
else
    bad "flat path yielded inadmissible $f0/$f1"
fi

# ---------------------------------------------------------------- case 2
# THE CASE THIS EXPERIMENT WAS BUILT FOR.
#
# Path A is the board: flat, and 3000/6000 is fine on it. Path B is a speaker
# with a deep notch at 3000 -- which is the laptop, measured in experiment 002.
# The incumbent pair must lose, and the winner must avoid 3000 entirely.
mkpath "$WORK/board.tsv"  board  -20 -20 ""
mkpath "$WORK/laptop.tsv" laptop -20 -55 "3000"
out=$("$BS" minimax "$WORK/board.tsv" "$WORK/laptop.tsv")
f0=$(echo "$out" | sed -n 's/^WINNER  \([0-9]*\) .*/\1/p')
f1=$(echo "$out" | sed -n 's/^WINNER  [0-9]* \/ \([0-9]*\) .*/\1/p')
if [ "$f0" != "3000" ] && [ "$f1" != "3000" ]; then
    ok "a notch at 3000 is avoided (winner $f0/$f1)"
else
    bad "winner $f0/$f1 uses a notched tone"
fi
if echo "$out" | grep -q 'minimax improvement: +'; then
    ok "the incumbent 3000/6000 is beaten, and by how much is reported"
else
    bad "no improvement over the incumbent was reported"
    echo "$out" | sed 's/^/       /'
fi

# ---------------------------------------------------------------- case 3
# MINIMAX IS NOT AVERAGE.
#
# 4200 and 6000 are superb on path a (-5) and dead on path b (-60). Every
# other tone is mediocre on both (-35).
#
# Averaging prefers the one-sided pair: it means -32.5 dB, which beats the
# mediocre pair's -35. Minimax must prefer the mediocre pair, scoring it -35
# against the one-sided pair's -60, because a stranger arrives on ONE path and
# does not get to average.
#
# The first version of this fixture put the flat level at -70 on one path and
# -20 on the other, which made every pair score -70 and tested nothing. A
# fixture whose cases are indistinguishable passes for the wrong reason.
mkpath "$WORK/a.tsv" a -35 -5  "4200 6000"
mkpath "$WORK/b.tsv" b -35 -60 "4200 6000"
out=$("$BS" minimax "$WORK/a.tsv" "$WORK/b.tsv")
score=$(echo "$out" | sed -n 's/^worst-path weaker-tone SNR: \(.*\) dB/\1/p')
f0=$(echo "$out" | sed -n 's/^WINNER  \([0-9]*\) .*/\1/p')
f1=$(echo "$out" | sed -n 's/^WINNER  [0-9]* \/ \([0-9]*\) .*/\1/p')
case "$f0 $f1" in
    *6000*|*4200*) bad "minimax chose $f0/$f1, which contains a one-sided tone" ;;
    *)             ok "minimax refused the one-sided tone (chose $f0/$f1)" ;;
esac
if [ "$score" = "55.00" ]; then
    ok "score is the worst path's weaker tone ($score dBFS)"
else
    bad "score $score is not the worst path's weaker tone SNR (55.00)"
fi

# ---------------------------------------------------------------- case 4
# Curves recorded at different input gains are not comparable, and the tool
# must refuse rather than produce a plausible winner.
# Selection is scored on SNR, from which a constant capture gain cancels
# exactly. So a mismatch must be REPORTED -- the raw dBFS column is still not
# comparable across paths -- without blocking an answer that does not depend
# on it. An earlier version refused outright, which would have blocked this
# campaign's real comparison: a board driven by a digital gain against a
# laptop driven by a render volume can never be matched exactly.
sed 's/^# capgain\t.*/# capgain\t0.270000/' "$WORK/board.tsv" > "$WORK/loud.tsv"
if "$BS" minimax "$WORK/board.tsv" "$WORK/loud.tsv" > "$WORK/o" 2>&1; then
    if grep -q 'different settings' "$WORK/o" && grep -q '^WINNER' "$WORK/o"; then
        ok "mismatched capture gains are reported, and SNR still decides"
    else
        bad "mismatch was neither reported nor scored"
    fi
else
    bad "mismatched capture gains blocked an SNR-based selection"
fi

# ---------------------------------------------------------------- case 5
# `slots` must refuse without the two level declarations, for the same reason.
if "$BS" slots /dev/null nolevels > "$WORK/o" 2>&1; then
    bad "slots ran without --capgain/--emitgain"
else
    grep -q 'required' "$WORK/o" && ok "slots requires the level declarations" \
        || bad "slots failed for the wrong reason"
fi

# ---------------------------------------------------------------- case 6
# The ladder generator and the analyser must agree. A synthetic ladder has no
# channel in it, so every tone must come back at the same level -- if they do
# not, the tool has a frequency-dependent bias and every curve it ever
# produces is partly a picture of itself.
"$BS" ladder "$WORK/l.wav" > /dev/null
"$BS" slots "$WORK/l.wav" synthetic --capgain 0.12 --emitgain 50 > "$WORK/l.tsv"
spread=$(awk -F'\t' '/^[0-9]/ { if (min == "" || $2 < min) min = $2;
                                if (max == "" || $2 > max) max = $2 }
                     END { printf "%.3f", max - min }' "$WORK/l.tsv")
rows=$(grep -c '^[0-9]' "$WORK/l.tsv")
if [ "$rows" -ne 27 ]; then
    bad "ladder round trip produced $rows tones, expected 27"
elif awk "BEGIN { exit !($spread < 0.05) }"; then
    ok "ladder round trip is flat to $spread dB across 27 tones"
else
    bad "ladder round trip varies by $spread dB with no channel present"
fi

# Leakage must be far below the tone in a synthetic ladder; if it is not, slot
# alignment is wrong and every over-air number would be a blend of neighbours.
worst=$(awk -F'\t' '/^[0-9]/ { if (min == "" || $6 < min) min = $6 }
                    END { printf "%.1f", min }' "$WORK/l.tsv")
if awk "BEGIN { exit !($worst > 60) }"; then
    ok "slot alignment is exact (worst own-vs-leak $worst dB)"
else
    bad "own-vs-leak fell to $worst dB on a synthetic ladder"
fi

echo
if [ "$FAIL" -ne 0 ]; then
    echo "SELF-TEST FAILED: $FAIL"
    exit 1
fi
echo "SELF-TEST PASSED"
