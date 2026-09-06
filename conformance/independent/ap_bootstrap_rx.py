"""
An independent AP-BOOTSTRAP-1 receiver.

WHAT "INDEPENDENT" MEANS HERE, AND WHAT IT DOES NOT

It shares no code with mcl-ap. Nothing is ported, translated or consulted from
src/ap_modem.c; every number in this file comes from spec/ap-bootstrap-1.md,
and the section it comes from is named at the point it is used. If a constant
were missing from the specification this file could not have been written, and
that is the test being run.

It is NOT organisationally independent: the same author wrote the specification
this was written from. So it can demonstrate that the document is
SELF-SUFFICIENT -- that no required parameter lives only in the reference
implementation -- and it cannot demonstrate that the document is
UNAMBIGUOUS to a reader who has never seen the reference. Section 11 of the
profile asks for the second thing as well, and this file is not it. Saying so
here is cheaper than letting a reader assume the stronger claim.

Python is deliberate. It cannot accidentally link the C library and cannot
share a rounding convention with it by accident.

Usage:
    python ap_bootstrap_rx.py <vector directory>
"""

import cmath
import math
import os
import struct
import sys

# ---------------------------------------------------------------- §3, §5
SAMPLE_RATE = 48000            # §3
BAUD = 300                     # §3
SAMPLES_PER_SYMBOL = 160.0     # §3: "exactly 160 samples per symbol"
TONE_0 = 3000.0                # §3
TONE_1 = 6000.0                # §3
CHIRP_START = 2000.0           # §3.1
CHIRP_END = 6000.0             # §3.1
CHIRP_SECONDS = 0.2            # §3.1
TRAINING_SYMBOLS = 16          # §3.1
HEADER_BYTES = 3               # §5
MAX_PAYLOAD = 64               # §4
DETECTION_THRESHOLD = 0.40     # §6.1
CHIRP_ENERGY_PER_SAMPLE = 0.40 # §3.1

# §6.4
REFINE_SPAN = 0.6
REFINE_STEP = 0.01


class Refused(Exception):
    """A refusal with a reason. §6 and the vector manifest require the reason
    to survive: 'heard nothing' and 'heard something and could not read it'
    are different problems and only the second means a link is nearly working."""

    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def read_wav(path):
    """Minimal RIFF reader. 48 kHz monaural signed 16-bit PCM only."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos = 12
    fmt = None
    samples = None
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk_id == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif chunk_id == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    if fmt is None or samples is None:
        raise ValueError("missing fmt or data chunk")
    _, channels, rate, _, _, bits = fmt
    if channels != 1 or rate != SAMPLE_RATE or bits != 16:
        raise ValueError("expected 48 kHz monaural 16-bit PCM")
    count = len(samples) // 2
    return list(struct.unpack_from("<%dh" % count, samples, 0))


def chirp_reference():
    """§3.1. Phase accumulated in double precision, which the specification
    requires: the quadratic term reaches 2*pi*6000*0.2 radians and a
    single-precision mantissa loses the fractional part of an angle that size
    before the chirp ends."""
    n = int(round(CHIRP_SECONDS * SAMPLE_RATE))
    k = (CHIRP_END - CHIRP_START) / CHIRP_SECONDS
    out = []
    for i in range(n):
        t = i / SAMPLE_RATE
        phase = 2.0 * math.pi * (CHIRP_START * t + 0.5 * k * t * t)
        out.append(complex(math.sin(phase), math.cos(phase)))
    return out


def acquire(pcm, reference):
    """§6.1. Normalised cross-correlation. Normalisation is required, not
    optional: an un-normalised correlation peaks on loud noise."""
    n = len(reference)
    if len(pcm) < n:
        raise Refused("not acquired")

    # The correlation is evaluated on a decimated subset for speed. Every term
    # of the normalisation must then use THE SAME SUBSET: taking the signal and
    # reference energies over the full window while summing the correlation
    # over a quarter of it divides a partial numerator by a whole denominator,
    # and the result is a correlation that never reaches the 0.40 threshold no
    # matter how good the signal is. That mistake reports a perfect frame as
    # "not acquired", which looks exactly like a dead channel.
    stride = 4
    taps = range(0, n, stride)
    ref_energy = math.sqrt(sum(abs(reference[i]) ** 2 for i in taps))

    best_value = 0.0
    best_index = 0

    # A coarse pass, then a fine pass around the winner: stepping by 8 costs a
    # little peak accuracy and lets a pure-Python search finish, and the fine
    # pass recovers the sample.
    for coarse in (8, 1):
        if coarse == 8:
            candidates = range(0, len(pcm) - n, 8)
        else:
            lo = max(0, best_index - 8)
            hi = min(len(pcm) - n, best_index + 8)
            candidates = range(lo, hi + 1)
        for start in candidates:
            acc = 0j
            energy = 0.0
            for i in taps:
                sample = float(pcm[start + i])
                energy += sample * sample
                acc += sample * reference[i].conjugate()
            if energy == 0.0:
                continue
            value = abs(acc) / (math.sqrt(energy) * ref_energy)
            if value > best_value:
                best_value = value
                best_index = start

    return best_index, best_value


def goertzel(pcm, start, length, frequency):
    """Energy at one frequency over one window. §6.3."""
    if start < 0 or start + length > len(pcm):
        raise Refused("sync")
    omega = 2.0 * math.pi * frequency / SAMPLE_RATE
    coeff = 2.0 * math.cos(omega)
    s1 = 0.0
    s2 = 0.0
    for i in range(length):
        s0 = float(pcm[start + i]) + coeff * s1 - s2
        s2 = s1
        s1 = s0
    return s1 * s1 + s2 * s2 - coeff * s1 * s2


def soft(pcm, start, length):
    """log(E1/E0). §6.3."""
    e0 = goertzel(pcm, start, length, TONE_0)
    e1 = goertzel(pcm, start, length, TONE_1)
    return math.log((e1 + 1e-12) / (e0 + 1e-12))


def estimate_bias(pcm, fsk_start):
    """§6.2. The training symbols are 0x55 twice: alternating 0 and 1, so the
    bias is the midpoint of the two classes' log ratios.

    The specification is explicit that this is not zero and must be estimated:
    an acoustic path does not present two frequencies equally."""
    length = int(SAMPLES_PER_SYMBOL)
    zeros = []
    ones = []
    for bit in range(TRAINING_SYMBOLS):
        start = fsk_start + int(bit * SAMPLES_PER_SYMBOL)
        value = soft(pcm, start, length)
        # 0x55 is 0101_0101, MSB first: even indices are 0, odd are 1.
        (zeros if bit % 2 == 0 else ones).append(value)
    return (sum(zeros) / len(zeros) + sum(ones) / len(ones)) / 2.0


def crc16_ccitt_false(data):
    """§5. Polynomial 0x1021, init 0xFFFF, no reflection, no final XOR."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                else (crc << 1) & 0xFFFF
    return crc


def demodulate(pcm, payload_start, sps, bias, byte_count):
    """§6.3. MSB first within each byte, §3."""
    out = bytearray()
    length = int(round(sps))
    for index in range(byte_count):
        value = 0
        for bit in range(8):
            position = index * 8 + bit
            start = payload_start + int(position * sps)
            if start + length > len(pcm):
                raise Refused("sync")
            value = (value << 1) | (1 if soft(pcm, start, length) - bias > 0.0
                                    else 0)
        out.append(value)
    return bytes(out)


def read_frame(pcm, payload_start, sps, bias):
    """§5 then §4. The order of checks is normative: a receiver MUST reject a
    declared length of 0 or above 64 BEFORE computing anything over it."""
    header = demodulate(pcm, payload_start, sps, bias, HEADER_BYTES)
    declared = header[0]
    received_crc = (header[1] << 8) | header[2]

    if declared == 0 or declared > MAX_PAYLOAD:
        raise Refused("payload")

    body_start = payload_start + int(HEADER_BYTES * 8 * sps)
    payload = demodulate(pcm, body_start, sps, bias, declared)
    if crc16_ccitt_false(payload) != received_crc:
        raise Refused("crc")
    return payload


def mean_margin(pcm, start, sps, bias, symbols):
    """§6.4. Scores the SIGNAL only -- never the expected payload -- which is
    what makes it something a receiver can actually run."""
    length = int(round(sps))
    total = 0.0
    counted = 0
    for bit in range(symbols):
        position = start + int(bit * sps)
        if position + length > len(pcm):
            break
        total += abs(soft(pcm, position, length) - bias)
        counted += 1
    if counted == 0:
        return -1e30
    return total / counted


def decode(path):
    pcm = read_wav(path)
    reference = chirp_reference()
    index, correlation = acquire(pcm, reference)
    if correlation < DETECTION_THRESHOLD:
        raise Refused("not acquired")

    fsk_start = index + len(reference)
    bias = estimate_bias(pcm, fsk_start)
    payload_start = fsk_start + int(TRAINING_SYMBOLS * SAMPLES_PER_SYMBOL)

    try:
        return read_frame(pcm, payload_start, SAMPLES_PER_SYMBOL, bias)
    except Refused as first:
        # §6.4 is MANDATORY: a conforming receiver retries with a refined
        # symbol rate before reporting failure.
        best_sps = SAMPLES_PER_SYMBOL
        best_score = -1e30
        steps = int(round(2 * REFINE_SPAN / REFINE_STEP)) + 1
        for step in range(steps):
            candidate = SAMPLES_PER_SYMBOL - REFINE_SPAN + step * REFINE_STEP
            start = fsk_start + int(TRAINING_SYMBOLS * candidate)
            score = mean_margin(pcm, start, candidate, bias,
                                (HEADER_BYTES + 16) * 8)
            if score > best_score:
                best_score = score
                best_sps = candidate
        if abs(best_sps - SAMPLES_PER_SYMBOL) < 1e-9:
            raise
        start = fsk_start + int(TRAINING_SYMBOLS * best_sps)
        try:
            return read_frame(pcm, start, best_sps, bias)
        except Refused:
            raise first


EXPECTED = {
    "01-presence-10b": "accept",
    "02-transport-accept-16b": "accept",
    "03-transport-offer-17b": "accept",
    "04-refuse-bad-crc": "crc",
    "05-refuse-zero-length": "payload",
    "06-refuse-length-over-cap": "payload",
    "07-refuse-truncated": "sync",
    "08-refuse-no-preamble": "not acquired",
    "09-refuse-silence": "not acquired",
}


def main():
    if len(sys.argv) != 2:
        print("usage: ap_bootstrap_rx.py <vector directory>")
        return 2
    directory = sys.argv[1]
    failures = 0
    checked = 0

    for name in sorted(EXPECTED):
        path = os.path.join(directory, name + ".wav")
        if not os.path.exists(path):
            print("  MISSING %s" % name)
            failures += 1
            continue
        checked += 1
        want = EXPECTED[name]
        try:
            payload = decode(path)
            got = "accept"
            detail = payload.hex().upper()
        except Refused as refusal:
            got = refusal.reason
            detail = ""
        if got == want:
            print("  ok    %-28s %-14s %s" % (name, got, detail))
        else:
            print("  FAIL  %-28s expected %-14s got %s" % (name, want, got))
            failures += 1

    print("\n%d vectors, %d failed" % (checked, failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
