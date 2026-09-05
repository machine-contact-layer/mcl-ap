# MCL-AP Experimental Program

MCL-AP is research-driven. No mandatory physical parameters should be frozen until the bootstrap, channel, and cross-layer experiments are reproducible.

## Experiment 000 — origin capture characterization

Purpose:

- characterize the accidental symbol-to-tone / failed-TTS recording that motivated the acoustic branch
- document why it is not a valid ggwave packet or a calibrated channel measurement
- retain it only as an adverse-capture and historical baseline

Status: analysis exists outside this repository and will be migrated with source/received artifacts when binary artifact handling is finalized.

## Experiment 001 — known-waveform channel estimation

Retain both transmitted `X(t)` and received `Y(t)`.

Source waveform should include:

1. silence for noise estimation
2. synchronization preamble
3. known logarithmic chirp across the available audio band
4. time-frequency discovery tiles
5. controlled multitone probe
6. known short frames in several bands
7. candidate Tier-0 frames
8. ending silence

Required outputs:

- transfer-function estimate
- noise PSD
- usable frequency mask
- impulse response / delay spread
- frequency-selective attenuation
- sample-clock/frequency offset
- clipping/nonlinearity indicators

## Experiment 002 — real path characterization and frequency-diverse bootstrap

Two parts. Part A is implemented and has a first result; part B is not started.

### Part A — real transducer path characterization (IMPLEMENTED)

Measure the frequency response of an actual speaker → air → microphone pair with a
stepped-tone probe, so band and profile choices come from measurement rather than intuition.
There is no universal speaker/microphone curve, so every claim here is scoped to one measured
device pair and one direction.

Implementation and evidence: `002-path-characterization/`.

First result (2026-09-02, laptop Realtek speaker → DFR1154 PDM microphone, 3 captures):
the Experiment 001 FSK pair straddles a notch — the 5 kHz mark tone measures 19–21 dB below
the 3 kHz space tone, while 5500/6000/8000/9000 Hz all sit at or above the 3 kHz reference and
9 kHz measures 10–12 dB stronger. This explains the residual 2/10 E3 payload failures and
shows they are a band-selection artifact rather than a limit of the path.

### Part B — frequency-diverse bootstrap (NOT STARTED)

Compare:

- fixed single-band bootstrap
- sequential frequency sweep
- time-frequency tiled superframe
- chirp-assisted acquisition
- erasure-tolerant tile identity

Bootstrap candidates must be evaluated under equal resources on measured paths, using the
part A characterization as input, not on assumed spectrum.

## Experiment 003 — profile convergence

Compare fixed PHY versus automatic directional convergence across measured and synthetic channels.

Metrics:

- convergence time
- selected profile quality
- PER / verified goodput
- fallbacks
- asymmetry between directions

## Experiment 004 — source/wire compression

Inputs: identical MCL semantic scenario corpus.

Baselines:

- JSON
- CBOR
- typed binary
- fixed bit fields
- context
- delta
- context + delta
- context + entropy coding

Metric: transmitted bits versus semantic/task distortion and context recovery cost.

## Experiment 005 — semantic-aware channel protection

Under identical physical channel resources compare:

```text
generic serialized bytes + uniform protection
```

against:

```text
MCL canonical semantics + priority-aware protection
```

Primary metric: mandatory semantic recovery, not raw packet recovery alone.

## Experiment 006 — mobility and Doppler

Sweep relative velocity, carrier region, symbol duration, and candidate PHY family.

Measure synchronization loss, frequency tracking, BER/PER, and reacquisition.

## Experiment 007 — multi-node contact

Evaluate broadcast fan-out, simultaneous discovery, contention, acknowledgement scheduling, and authority/hazard broadcast behavior.

## Reproducibility requirements

Every physical experiment should retain:

- exact source samples
- received raw samples where possible
- sample rate / bit depth / channel count
- device identities and audio modes
- distance and orientation
- room/environment description
- volume/gain settings where known
- preprocessing/AGC/AEC/noise-suppression state
- code commit
- exact decoder/profile configuration

Do not publish bitrate claims without the accounting method and reliability criterion.

## Experiment 003 — band-informed FSK candidate (IMPLEMENTED)

Takes the Experiment 002 part A measurement and changes exactly one variable in the
Experiment 001 waveform: the FSK mark tone moves from 5000 Hz, which the measurement found
19-21 dB down inside a notch, to 6000 Hz, which sits at or above the 3 kHz reference on both
receivers available.

In a single controlled session on one rig, that change took exact Wire and semantic recovery
from 0/10 to 9/10 on the DFR1154 receiver and from 0/10 to 4/10 on the laptop Realtek
receiver, against a 3000/5000 control captured in the same session.

The result is about method, not about 6 kHz: profiles must be selected from measurement of the
actual path. AP-B0 remains NOT selected, and 4/10 on the laptop receiver shows a fixed binary
FSK pair is still fragile. Implementation and evidence: `003-band-informed-candidate/`.

## Experiment 008 — the embedded node (IMPLEMENTED, E4)

The first experiment in which **MCL itself runs on the microcontroller** rather
than on a laptop with the board as an instrument.

The DFR1154 builds a major-1 Tier-0 `PRESENCE` with `mcl-wire`, wraps it in a
major-1 Link frame with `mcl-link`, modulates it with the portable AP candidate
modem and emits it — and in the other direction it acquires, demodulates,
verifies the CRC, decodes the frame, decodes the object inside it, and reports
the field values it read. In `host-to-board` no host is in the loop at all.

Result, 2026-09-04, ten trials per cell, one session:

| Direction | Payload | Acquired | Exact recovery |
|---|---|--:|--:|
| board → host | 24-byte Link frame | 10/10 | 2/10 |
| board → host | 10-byte Wire object | 10/10 | 9/10 |
| host → board | 10-byte Wire object | 10/10 | 6/10 |
| host → board | 24-byte Link frame | 10/10 | 7/10 |

This is **portability and end-to-end operation**, not independent
implementation: the board compiles the same source the host compiles, with a
different toolchain for a different architecture. `008-embedded-node/README.md`
states the boundary, the on-board cost, and the three harness defects that
produced numbers looking like acoustic failures before any acoustic result was
real.

AP-B0 remains **NOT selected**.

Numbered 008 because 004 through 007 are already allocated to planned
experiments above. An allocated number is not reused, for the same reason a
registry value is not reused: somebody may already have referred to it.
(003 does appear twice in this index — the planned profile-convergence study
and the implemented band-informed FSK candidate. That collision predates this
experiment, and adding a third one would not fix it.)

## Experiment 009 — an Android handset as the acoustic transmitter (IMPLEMENTED, E3)

`009-android-acoustic-peer/`

A third loudspeaker, and the first one nobody here chose: a consumer phone
running a vendor Android build. The phone plays the MCL-AP waveform; a laptop
running the same `ap_modem.c` the DFR1154 runs demodulates the room.

Result, 2026-09-05, ten trials per cell, one session:

| Payload | Acquired | Exact recovery |
|---|--:|--:|
| 10-byte Wire object | 10/10 | 4/10 |
| 24-byte Link frame | 10/10 | 1/10 |

Acquisition correlation ran 0.73-0.87 in every trial, so no failure is a missed
frame; every failure is a payload bit error. A recovered trial decodes a
major-1 Link frame carrying a major-1 `PRESENCE` field by field.

**One direction only.** The phone emits and never receives: capture on Android
requires `RECORD_AUDIO`, `/dev/snd` is `system:audio`, and the adb shell user is
not in the `audio` group, so no shell binary can open the microphone. That is
Android's permission model and not a property of the modem, and the experiment
says so rather than leaving a reader to infer it from a missing row.

The same 2.2x payload growth that collapsed Experiment 008's board→host
direction (9/10 to 2/10) collapses this one (4/10 to 1/10). Two unrelated
transmitters, the same shape of degradation: the payload length is doing this.

**Not a usable link, and not claimed as one.** AP-B0 remains **NOT selected**
and MCL-AP remains Experimental.

## Experiment 010 — bit-error structure of the retained corpus

Offline. No transmission, no hardware, no new claim about a link.

Experiments 008 and 009 both recorded the same shape of failure — recovery
collapsing as the payload grew, from two unrelated transmitters — and neither
said *why*. Experiment 010 measures it, because
`spec/ap-bootstrap-requirements-v0.1.md` §6 forbids choosing a coding scheme for
`AP-BOOTSTRAP-1` before the error structure is known: the four plausible causes
call for four remedies that are not interchangeable, and adding FEC to a timing
problem costs airtime and buys nothing.

Four cells, exact ground truth taken from each campaign's own run log:

| Payload | BER | Clean | Errors in 2nd half | Timing-removable |
|--:|--:|--:|---|--:|
| 10 B | 0.192% | 9/10 | all | 100% |
| 11 B | 0.357% | 9/10 | all | — |
| 24 B | 3.889% | 3/10 | 3.9x the 1st half | 53.6% |

Errors are **isolated rather than bursty**, **asymmetric** (77-100% one way),
**low-margin**, and **concentrated in the frame tail**. 2.4x the payload gives
20x the bit error rate.

So interleaving buys little and the burst assumption behind it is wrong; timing
recovery is the largest single lever at 24 bytes but caps at 53.6%; and the rest
is genuine marginal SNR needing modest FEC sized against tail density.

**The dominant lever is none of those. It is frame length**, and
`AP-BOOTSTRAP-1` already caps its payload at the 17-byte Tier-0 ceiling — a
bound adopted so a bootstrap cannot carry a credential, which independently
places the profile in the regime where the modem already works.

Selects nothing. AP-B0 remains **NOT selected** and MCL-AP remains Experimental.
