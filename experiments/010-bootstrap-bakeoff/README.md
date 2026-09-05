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
nothing to do with the channel: the profile has **no credential-bearing and no
opaque-payload facility**, and a bare Ed25519 signature is 64 bytes. (§2.2 there
states the property precisely: it removes the mechanism a conforming use would
reach for and prohibits senders from encoding sensitive material into bootstrap
fields. It is not a claim that a frame cannot leak — no wire format decides
covert encoding.)

This measurement arrives at the same bound from the opposite direction. **10
bytes is 0.192% BER and 9/10 recovery; 24 bytes is 3.889% and 3/10.** The cap
places the profile **below the measured failure regime**, and the 24-byte regime
that motivated a search for FEC is one `AP-BOOTSTRAP-1` never enters.

Two independent arguments converging on one bound is worth more than either.

### Below the failure regime is not the same as inside a working one

The bootstrap exchange is three objects, not one:

```text
PRESENCE           10 bytes   measured    0.192% BER, 9/10
TRANSPORT_OFFER    17 bytes   NOT MEASURED
TRANSPORT_ACCEPT   16 bytes   NOT MEASURED
```

The degradation this experiment found is **superlinear** -- 2.4x the payload for
20x the error rate -- so interpolating from 10/11 bytes to 16/17 is exactly the
inference this data forbids. The worst-case bootstrap objects are the ones
nobody has measured, and until they are the FEC decision is deferred rather than
closed.

The eventual criterion is also not raw BER but end-to-end contact success within
a bounded retry window. 90% one-shot recovery is adequate with a cheap retry and
useless if contention makes every retry expensive, so this closes together with
the contention campaign or not at all.

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

## 9. Experiment 010b: the two bootstrap objects nobody has transmitted

§6 said the profile sits *below* the measured failure regime and explicitly
refused to say it sits inside a working one, because `TRANSPORT_OFFER` (17 B)
and `TRANSPORT_ACCEPT` (16 B) have never been sent over air and the degradation
is superlinear.

`timing_tolerance.c` closes as much of that gap as can be closed offline. It
invents **no noise model and no SNR** — §7 of the requirements forbids treating
synthetic impairment as selection evidence. It measures one deterministic
property, the timing tolerance of a frame as a function of its length, using the
**real** major-1 objects encoded by `mcl-wire`, and combines it with the
symbol-rate errors the shipped receiver **actually produced** on the twenty
retained board-to-host captures.

| Object | bytes | bits | \|sps\| tolerance | drift at failure | of a symbol | predicted clean |
|---|--:|--:|--:|--:|--:|--:|
| `PRESENCE` | 10 | 104 | 0.570 | 59.3 | 0.370 | 20/20 |
| `TRANSPORT_ACCEPT` | 16 | 152 | 0.410 | 62.3 | 0.389 | 16/20 |
| `TRANSPORT_OFFER` | 17 | 160 | 0.390 | 62.4 | 0.390 | **14/20** |

### 9.1 The mechanism, stated as a law

Tolerance times bits — the accumulated timing error when the frame stops
decoding — is 59.3, 62.3, 62.4 samples. **Near-constant, at 0.37–0.39 of a
symbol.**

That is Experiment 010's diagnosis expressed as a design rule rather than an
observation:

```text
a frame fails when accumulated drift reaches ~0.38 of a symbol
usable length  ~=  60 / |sps error|   bits
```

### 9.2 What it predicts, and why it is uncomfortable

**The worst-case bootstrap object is marginal, not comfortable.**
`TRANSPORT_OFFER` is predicted clean in 14 of 20 — **70%** — from the timing
term *alone*, with everything else held perfect. Experiment 010 measured that
roughly half the real errors at 24 bytes were **not** timing-removable, so the
physical campaign should be expected to do worse than this line, not better.

A bootstrap exchange is `PRESENCE` → `OFFER` → `ACCEPT`. A 70% ceiling on its
largest object, before noise and before contention, is not a working rendezvous.
So §6 was right not to claim the profile sits in a proven-good regime, and the
FEC decision stays open rather than closed by short frames.

### 9.3 It also says which remedy

The estimator searches symbol rate in 0.05 steps across ±0.5 around the true
160.0, and still returned values 0.4 and 0.45 away from truth on real captures.
**The correct answer was inside the search grid and was not chosen.** That makes
this a scoring problem, not a resolution problem: 16 training bits is too short
a baseline to pin a rate.

So the indicated fix is **a longer timing reference or a mid-frame pilot — not a
finer search and not, in the first instance, coding.** A block code sized for a
30% frame loss driven by drift would be paying airtime to carry a problem that
better acquisition removes.

That is a hypothesis with a number attached, which is what the physical campaign
is for. It is not a selection, and `AP-BOOTSTRAP-1` remains unspecified.

## 10. Experiment 010c: the remedy, tested on real captures

§9.3 said the fix is a longer estimation baseline, not a finer search. That was
a hypothesis with a number attached. `timing_remedy.c` tests it on the retained
captures before any hardware campaign is spent on it.

The candidate estimator is **blind** — it uses no ground truth, so a receiver
can actually run it. It maximises the mean decision margin over the whole frame:

```text
score(sps) = mean over bits of | log_ratio(bit, sps) - bias |
```

At the correct rate every symbol is sampled near its centre and the tones
separate cleanly; at a wrong rate the later symbols straddle boundaries and the
margin falls. That uses ~160 bits of evidence instead of 16.

**It is not the `--sweep` diagnostic of §2.** That scores against the known
payload and is an upper bound no receiver can reach. This scores against nothing
but the signal; the payload is used only to check the CRC afterwards.

| Cell | payload | CRC before | CRC after | mean \|sps err\| |
|---|--:|--:|--:|---|
| 008 board→host frame | 24 B | 3/10 | **5/10** | 0.155 → 0.045 |
| 008 board→host wire | 10 B | 9/10 | **10/10** | 0.145 → 0.058 |
| 003 board→laptop | 11 B | 9/10 | **10/10** | 0.250 → 0.045 |

**Every cell improves, on two different rigs.** 21/30 to 25/30 overall, and the
hardest cell nearly doubles. The diagnosis in §9.3 holds: it was a scoring
problem, and 160 bits of evidence beats 16.

### 10.1 The residual is probably not error

After refinement the deviation from the nominal 160.0 settles at 0.045–0.058 in
all three cells rather than going to zero.

That is expected. Transmitter and receiver have independent crystals, so the
**actual** received symbol rate is 160.0 times a clock ratio and is not exactly
160.0. Trial 03 of the 24-byte cell is the visible case: its deviation from
nominal grew 0.000 → 0.070 while its CRC went from failing to passing. Against
nominal that reads as worse; against what the capture actually contains it is
right.

So "estimator error against 160.0" conflates estimator error with genuine clock
offset, and only the first is fixable by estimation. §9's tolerance law is
unaffected — it bounds accumulated drift regardless of cause — but the *budget*
now has two terms, and a bootstrap profile must leave room for the one it cannot
estimate away.

### 10.2 What it changes, and what it does not

It makes the timing remedy a measured improvement rather than a plausible one,
which is what the physical campaign needed before committing airtime to it. It
does **not** show what a rig does with a receiver running this: these are
retained captures re-analysed, not a new link.

The 16- and 17-byte objects remain unmeasured over air, contention remains
unmeasured, no waveform is selected, and `AP-BOOTSTRAP-1` remains unspecified.
