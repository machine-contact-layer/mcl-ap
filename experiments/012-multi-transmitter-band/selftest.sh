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
if echo "$out" | grep -q 'ranking difference: +'; then
    ok "the incumbent 3000/6000 is beaten, and by how much is reported"
else
    bad "no improvement over the incumbent was reported"
    echo "$out" | sed 's/^/       /'
fi
# BOTH terms must be reported, not just the one that ranked. A single number
# hid the fact that a lone bin was deciding whole bands.
if echo "$out" | grep -q 'tone_min' && echo "$out" | grep -q 'chirp_mean' &&
   echo "$out" | grep -q 'chirp_min'; then
    ok "the tone term and the chirp term are reported separately"
else
    bad "the score terms are not reported separately"
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
score=$(echo "$out" | sed -n 's/^  tone_min *\([0-9.-]*\) dB.*/\1/p')
f0=$(echo "$out" | sed -n 's/^WINNER  \([0-9]*\) .*/\1/p')
f1=$(echo "$out" | sed -n 's/^WINNER  [0-9]* \/ \([0-9]*\) .*/\1/p')
case "$f0 $f1" in
    *6000*|*4200*) bad "minimax chose $f0/$f1, which contains a one-sided tone" ;;
    *)             ok "minimax refused the one-sided tone (chose $f0/$f1)" ;;
esac
if [ "$score" = "55.00" ]; then
    ok "the tone term is the worst path's weaker tone ($score dB)"
else
    bad "tone_min $score is not the worst path's weaker tone SNR (55.00)"
fi

# ---------------------------------------------------------------- case 3b
# A SINGLE BIN MUST NOT DECIDE A BAND.
#
# The two FSK tones and the acquisition chirp fail differently: a null on a
# tone is a bit error every symbol, a null inside the sweep costs a fraction of
# a correlation peak. Folding both into one min() let ONE bin set the score for
# the whole pair -- which is not a hypothetical, it is what put the incumbent
# 3000/6000 at 32.64 dB in the published table when its two tones measure
# 55.75, on the strength of one bin at 3600 Hz.
#
# Here every tone is flat at 55 dB SNR except a single deep notch at 5400,
# which no admissible pair can use as a tone but many must sweep through.
#
# The assertion is made on the INCUMBENT line rather than on the winner,
# deliberately: a search that CAN avoid the notch should avoid it, and this
# fixture leaves it room to. The incumbent 3000/6000 has no such choice -- its
# sweep is fixed at 2000-6000 and crosses the notch -- which is exactly the
# real case, where one bin at 3600 Hz on the host speaker was reported as the
# incumbent scoring 32.64 dB while its two tones measure 55.75.
mkpath "$WORK/onebin.tsv" onebin -35 -90 "5400"
out=$("$BS" minimax "$WORK/onebin.tsv")
line=$(echo "$out" | sed -n 's/^  rank \(.*\)$/\1/p')
irank=$(echo "$line" | sed -n 's/^\([0-9.-]*\) dB .*/\1/p')
icmin=$(echo "$line" | sed -n 's/.*chirp_min \([0-9.-]*\)].*/\1/p')
itone=$(echo "$line" | sed -n 's/.*tone_min \([0-9.-]*\) .*/\1/p')
if [ -n "$irank" ] && [ -n "$icmin" ] && [ "$irank" != "$icmin" ]; then
    ok "one dead bin ($icmin dB) does not become the band's rank ($irank dB)"
else
    bad "the rank collapsed to the single worst swept bin (rank $irank, min $icmin)"
    echo "$out" | sed 's/^/       /'
fi
if [ "$itone" = "55.00" ]; then
    ok "and the tone term is unharmed by it ($itone dB)"
else
    bad "the tone term was $itone, expected 55.00"
fi

# ---------------------------------------------------------------- case 3c
# A RANKING IS NOT A VERDICT.
#
# A minimax will crown the best pair in a set where every pair is unusable and
# say so in the same words it uses for a good one. Whether a band can carry a
# frame is a separate question, so the tool must ask for the floor and must not
# invent one.
out=$("$BS" minimax "$WORK/flat.tsv")
if echo "$out" | grep -q 'NO VIABILITY FLOOR GIVEN'; then
    ok "with no floor the output says it has ranked and not judged"
else
    bad "a relative ranking was presented without saying so"
fi
#
# `set -e` OFF AROUND A COMMAND THAT IS SUPPOSED TO FAIL.
#
# The floor case exits non-zero on purpose, and under `set -e` a command
# substitution that fails aborts the whole script -- so this file ran, printed
# the checks above, and STOPPED, with no summary line and no failure. Every
# check after it silently did not happen. A self-test that can end without
# saying so is the same defect as the build script that ran a stale binary,
# which is why check-test-harnesses.sh exists.
set +e
out=$("$BS" minimax --floor 20 "$WORK/flat.tsv"); rc=$?
set -e
if [ "$rc" = "0" ] && echo "$out" | grep -q 'VIABLE at the stated floor'; then
    ok "a band above the stated floor is reported viable"
else
    bad "a band above the floor was not reported viable (rc $rc)"
fi
set +e
out=$("$BS" minimax --floor 90 "$WORK/flat.tsv"); rc=$?
set -e
if [ "$rc" != "0" ] && echo "$out" | grep -q 'NOT VIABLE'; then
    ok "the best pair in an unusable set is refused, not crowned"
else
    bad "an unusable winner passed the floor (rc $rc)"
    echo "$out" | sed 's/^/       /'
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
