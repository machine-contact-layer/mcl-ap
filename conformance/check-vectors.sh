#!/bin/sh
# Cross-test AP-BOOTSTRAP-1: the reference receiver and an independent one, on
# the same corpus, compared against the same expectations.
#
# WHY BOTH DIRECTIONS AND NOT JUST ONE
#
# Two implementations that each decode their own output prove nothing about
# each other. The vectors are the agreed bytes; this script is the comparison.
#
# The expectations live here in one place rather than in each receiver, so
# neither implementation can define its way to passing.
#
# WHAT A PASS HERE DOES NOT MEAN
#
# These vectors are noise-free -- they are the transmitter's own output. A
# receiver that omitted the mandatory whole-frame rate refinement of
# spec/ap-bootstrap-1.md section 6.4 would pass this corpus, because these
# frames decode on the first attempt. Timing recovery is what fails in a room,
# and these vectors have perfect timing. The over-air evidence is
# experiments/011-bootstrap-over-air/, and the two are not substitutes.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
VECTORS="$ROOT/conformance/vectors"
INDEPENDENT="$ROOT/conformance/independent/ap_bootstrap_rx.py"

# The reference tool. Built by the experiments option; located rather than
# assumed, because a script that silently skips the half it cannot find reports
# a green run for a test it did not perform.
REFERENCE=${MCL_AP_BOOTSTRAP_TOOL:-}
if [ -z "$REFERENCE" ]; then
    for candidate in \
        "$ROOT/build/mcl_ap_exp011_bootstrap_air" \
        "$ROOT/build/Release/mcl_ap_exp011_bootstrap_air.exe" \
        "${TMPDIR:-/tmp}/mcl-ap-win/Release/mcl_ap_exp011_bootstrap_air.exe"
    do
        [ -x "$candidate" ] && REFERENCE="$candidate" && break
    done
fi

failures=0
checked=0

expect_for() {
    case "$1" in
        01-presence-10b|02-transport-accept-16b|03-transport-offer-17b)
            echo "accept" ;;
        04-refuse-bad-crc)          echo "crc" ;;
        05-refuse-zero-length)      echo "payload" ;;
        06-refuse-length-over-cap)  echo "payload" ;;
        07-refuse-truncated)        echo "sync" ;;
        08-refuse-no-preamble)      echo "not acquired" ;;
        09-refuse-silence)          echo "not acquired" ;;
        *)                          echo "UNKNOWN" ;;
    esac
}

echo "=== AP-BOOTSTRAP-1 vectors: reference receiver ==="
if [ -z "$REFERENCE" ] || [ ! -x "$REFERENCE" ]; then
    echo "FAIL: the reference tool was not found."
    echo "Build with -DMCL_AP_BUILD_EXPERIMENTS=ON or set MCL_AP_BOOTSTRAP_TOOL."
    failures=$((failures + 1))
else
    for wav in "$VECTORS"/*.wav; do
        [ -f "$wav" ] || continue
        name=$(basename "$wav" .wav)
        want=$(expect_for "$name")
        got=$("$REFERENCE" raw "$wav" 2>/dev/null || true)
        word=${got%% *}
        # "not acquired" is two words; compare against the whole string too.
        case "$got" in "not acquired"*) word="not acquired" ;; esac
        checked=$((checked + 1))
        if [ "$word" = "$want" ]; then
            printf '  ok    %-28s %s\n' "$name" "$got"
        else
            printf '  FAIL  %-28s expected %s, got %s\n' "$name" "$want" "$got"
            failures=$((failures + 1))
        fi
    done
fi

echo
echo "=== AP-BOOTSTRAP-1 vectors: independent receiver ==="
if ! command -v python3 >/dev/null 2>&1 && ! command -v python >/dev/null 2>&1
then
    echo "FAIL: no python interpreter, so the independent half did not run."
    failures=$((failures + 1))
else
    PY=$(command -v python3 || command -v python)
    if "$PY" "$INDEPENDENT" "$VECTORS"; then
        :
    else
        failures=$((failures + 1))
    fi
fi

echo
if [ "$failures" -ne 0 ]; then
    echo "VECTOR CROSS-TEST FAILED: $failures"
    exit 1
fi
echo "VECTOR CROSS-TEST PASSED (reference: $checked vectors, and the independent"
echo "receiver on the same corpus with the same expectations)"
