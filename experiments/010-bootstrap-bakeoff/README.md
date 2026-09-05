# Experiment 010: bit-error structure of the retained corpus

Status: **Research.** Selects nothing. Measures the error structure that
`spec/ap-bootstrap-requirements-v0.1.md` §6 requires before any coding scheme
for `AP-BOOTSTRAP-1` can be chosen.

Date: 2026-09-05. Offline analysis of captures already retained; no new
transmission, no hardware, no new claim about a link.

## 1. The question

§6 of the requirements states that four plausible error structures call for four
different and non-interchangeable remedies:

```text
independent and symmetric  ->  block FEC pays
bursty                     ->  interleaving first; repetition wastes airtime
asymmetric                 ->  threshold placement, before any coding
growing with bit index     ->  timing recovery, and no coding fixes it
```

Adding FEC to a timing problem buys nothing and costs airtime, which is the
scarcest resource this bearer has. So the structure is measured first.

## 2. Method

`error_structure.c` instruments the shipped decoder rather than reimplementing
it — it includes `src/ap_modem.c` so it can read the per-bit soft value
`log_ratio(...) - bias`, which is internal. A second modem that merely resembled
the first would make every difference between them indistinguishable from a
result.

For each capture it acquires, estimates timing, then compares every demodulated
bit against the bits the transmitter actually emitted. Ground truth is exact:
the campaigns recorded the transmitted hex in their run logs.

`--sweep` re-decodes over a fine grid of symbol rate (159.0–161.0 in 0.005 steps)
and phase (±80 samples) and reports the best error count reachable. **It is a
diagnostic, not a receiver** — it scores using the known payload, which no real
receiver has. It answers one question: were these errors recoverable at a
different timing estimate, or did the signal not carry the bits?

## 3. Results

| Cell | Payload | Bits | Clean | BER | 1st half | 2nd half | Asymmetry | Isolated errors | Timing-removable |
|---|---:|---:|---:|---:|---:|---:|---|---|---:|
| 003 board→board | 11 B | 112 | 9/9 acq | 0.000% | — | — | — | — | — |
| 003 board→laptop | 11 B | 112 | 9/10 | 0.357% | 0.000% | 0.714% | 100% `1→0` | 4 of 4 | — |
| 008 board→host wire | 10 B | 104 | 9/10 | 0.192% | 0.000% | 0.385% | 100% `1→0` | 2 of 2 | 100% |
| 008 board→host frame | 24 B | 216 | 3/10 | 3.889% | 1.574% | 6.204% | 77.4% `1→0` | 58 of 67 runs | 53.6% |

Raw output in `evidence/error-structure-20260905/`.

## 4. What the structure is

**Errors are not bursty.** In the 24-byte cell, 58 of 67 error runs are single
bits; the longest is 4. In the three short cells every error is isolated.
*Interleaving buys very little, and the burst assumption behind it is wrong.*

**Errors are strongly asymmetric.** 77.4% in the 24-byte cell and 100% in all
three short cells run `1→0`. This is the tilted-path effect the decision
threshold already partially compensates: an acoustic path does not present two
frequencies equally, and Experiment 002 measured one 19–21 dB down in a notch.

**Errors are marginal, not gross.** Errored bits average |margin| 0.56–0.64
against 3.11–5.41 for correct bits, across every cell. Decisions are degrading
gradually, not being destroyed.

**Errors concentrate in the frame tail, in all four cells.** Every single error
in all three short cells falls in the second half. In the 24-byte cell the
second half is 3.9× worse than the first.

**And length hurts superlinearly.** 10 bytes to 24 bytes — 2.4× the payload —
takes BER from 0.192% to 3.889%, a factor of **20**. Recovery falls 9/10 to
3/10.

That combination is the signature of **residual symbol-rate error accumulating
across the frame**: the rate is estimated from 16 training bits on a ±0.5 grid at
0.05 resolution, and whatever error survives that estimate integrates linearly
with bit index. At 216 bits a residual of 0.4 samples/bit is 86 samples — over
half a symbol by the end of the frame.

## 5. But timing is only half of it

This is the finding that stops a premature conclusion.

The sweep removes **100%** of errors in the 10-byte cell and only **53.6%** in
the 24-byte cell. Trial 06 keeps 21 of its 35 errors at the best timing
reachable anywhere in the grid. Those bits are not recoverable by any timing
estimate; the signal did not carry them.

So the 24-byte cell is roughly half accumulated timing error and half genuine
marginal SNR, and the two need different remedies:

| Remedy | Verdict from this data |
|---|---|
| Interleaving | **little value** — errors are isolated, not bursty |
| Naive repetition | **poor value** — costs airtime linearly against a mostly-clean head |
| Longer training / finer rate search / mid-frame pilots | **highest single lever at 24 bytes** — bounded above by 53.6% |
| Decision threshold placement | **real and cheap** — 77–100% of errors run one way |
| Modest block FEC | **pays for the residual half**, sized against tail density, not mean BER |
| **Shorter frames** | **dominant** — see §6 |

## 6. The result that matters for AP-BOOTSTRAP-1

The strongest lever is not a coding choice. It is frame length, and the profile
already has the bound.

`AP-BOOTSTRAP-1` carries Wire major-1 Tier-0 objects and nothing else, so its
largest legal payload is the 17-byte Tier-0 ceiling and its typical payload is a
10-byte `PRESENCE`. That cap was chosen in
`spec/ap-bootstrap-requirements-v0.1.md` §2.1 for a structural reason that has
nothing to do with the channel: a bootstrap that *cannot* hold a credential
cannot leak one, and a bare Ed25519 signature is 64 bytes.

This measurement arrives at the same bound from the opposite direction. **10
bytes is 0.192% BER and 9/10 recovery; 24 bytes is 3.889% and 3/10.** The
security-motivated payload cap lands the profile in the regime where this modem
already works, and the 24-byte regime that motivated a search for FEC is a
regime `AP-BOOTSTRAP-1` never enters.

Two independent arguments converging on one bound is worth more than either.

## 7. What this does NOT establish

- **It does not select a waveform.** 3/6 kHz remains unselected and the registry
  still forbids standardising it because it rescued one campaign.
- **It does not measure contention**, which
  `spec/ap-bootstrap-requirements-v0.1.md` §3 makes a first-class requirement and
  which needs three or more machines.
- **It is one path family.** All four cells involve the same board and the same
  two rooms. The laptop-speaker notch that took a candidate from 9/10 to 0/10 is
  exactly the kind of effect this corpus cannot show.
- **53.6% is an upper bound, not an achievable gain.** The sweep uses ground
  truth. A real receiver estimating rate without it will recover less.
- **No link is claimed.** Recovery of 3/10 to 9/10 is not a usable link and
  v1.0 does not claim one.

## 8. Reproducing

```sh
gcc -std=c99 -O2 -Wall -I mcl-ap/include \
    -o errstruct mcl-ap/experiments/010-bootstrap-bakeoff/error_structure.c -lm

./errstruct --sweep --payload 10020DFB11540000043C \
    mcl-ap/experiments/008-embedded-node/evidence/e4-node-board-to-host-wire-20260904/*.wav
```

The target must not also link `mcl-ap`: it includes the implementation to reach
its static DSP helpers, and linking both would define them twice.
