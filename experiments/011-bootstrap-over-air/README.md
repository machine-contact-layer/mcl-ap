# Experiment 011: the bootstrap objects over air

Status: **E4 over-air result.** Measures the three major-1 bootstrap objects on
a real acoustic path between two devices with independent clocks, and selects
the receiver change that `spec/ap-bootstrap-requirements-v0.1.md` §6 left open.

Date: 2026-09-06. Rig: DFR1154 speaker → laptop microphone, board and host
running the same MCL sources compiled twice.

## 1. The question 010 could not answer

Experiment 010b derived a timing law from the retained corpus and **predicted**
that a 17-byte `TRANSPORT_OFFER` would decode cleanly in 14 of 20 attempts from
the timing term alone. 010c showed, offline on those same captures, that a
blind whole-frame rate refinement recovers most of the loss.

Neither is a measurement of the bootstrap objects. The corpus holds 10-, 11-
and 24-byte payloads and **nothing at 16 or 17**, and 010's own README records
that degradation with length is superlinear — so interpolating into the gap is
precisely the move that makes a number wrong. This experiment transmits the
actual objects.

| object | bytes |
|---|---:|
| `PRESENCE` | 10 |
| `TRANSPORT_ACCEPT` | 16 |
| `TRANSPORT_OFFER` | 17 |

Encoded by mcl-wire at `MCL_WIRE_STABLE_MAJOR`, so the bytes on the air are the
release's bytes rather than a hand-assembled lookalike.

## 2. Result

15 trials per cell, one session, one room, one level.

| cell | bytes | acquired | shipped receiver | blind refinement |
|---|---:|---:|---:|---:|
| `PRESENCE` | 10 | 15/15 | 9/15 | **15/15** |
| `TRANSPORT_ACCEPT` | 16 | 15/15 | 8/15 | **15/15** |
| `TRANSPORT_OFFER` | 17 | 15/15 | 4/15 | **14/15** |

**21 of 45 → 44 of 45.** Both columns come from the *same capture*, so their
difference is the estimator and nothing else — not a better room, not a better
session.

Raw output in `evidence/board-g75-20260906/`.

### 010b's prediction was optimistic, and that was expected

010b predicted 14/20 (70%) for the 17-byte object from the timing term alone,
with everything else perfect, and said a real rig should do worse. It did:
**4/15 (27%)**. The prediction was an upper bound and is now measured as one.

## 3. What was selected, and what was rejected

The refinement is now in `src/ap_modem.c`, **on the retry path only**: a frame
that decodes on the first attempt costs nothing extra, and the change is
therefore monotone — it can turn a failure into a success and can never turn a
success into a failure, because a success has already returned. That also
matters for the ESP32-S3, where the ~120-candidate search is far more expensive
than one demodulation pass.

**Forward error correction is rejected, on evidence.** 010 measured the error
structure first: errors are isolated rather than bursty (58 of 67 runs single
bits), asymmetric (77–100% `1→0`), low-margin, and concentrated in the frame
tail. That is a timing signature, not independent bit noise, and a block code
sized for a ~30% frame loss would pay airtime to carry a problem better
acquisition removes. This experiment closes the question: acquisition removed it.

**Regression check, offline, on the retained corpus** — no new transmission:

| retained cell | recorded | with the refined receiver |
|---|---:|---:|
| 008 board→host wire, 10 B | 9/10 | **10/10** |
| 008 board→host frame, 24 B | 3/10 | **5/10** |

Which is exactly what 010c predicted offline, and nothing regressed.

## 4. Instrument faults found, and how they were caught

Three, each of which produced numbers that looked like channel results.

**Clipping.** The first board run recorded every capture at a peak of exactly
0.0 dB. That is not a strong signal but a clipped one: the ADC ran out of range,
the tone tops were flattened, and the harmonics that manufactures are then
decided against by the demodulator. It degrades recovery *while looking like a
strong signal*, which makes it more dangerous than silence. The rig now
converges the input gain into a window before any trial is counted, and marks
any 0.0 dB capture `CLIPPED` in the log.

**A transmitter transient that does not scale with emission gain.** Driving the
board quieter and raising the input gain to compensate eventually clips on the
amplifier's turn-on click rather than on the signal: at capture gain 0.271,
three consecutive trials peaked at −0.3 dB with the room measured at −38.8 dB
mean, and correlation fell from 0.86 to 0.72. The input gain is now capped.

**A vtable slot that mutes the machine.** `IAudioEndpointVolume` puts
`GetMasterVolumeLevel` — decibels — at slot 6 and `GetMasterVolumeLevelScalar`
at slot 7. Declaring only the scalar getter puts it on slot 6, so a "scalar"
read returns a dB value, which clamps to 0, and restoring that 0 mutes the
endpoint. `audio-endpoint.ps1` declares the dB getter purely to hold its slot.

## 5. What this does not establish

**One session, one room, one distance, one pair of devices.** These numbers
describe this path. They are sufficient to *select* the receiver change,
because the selection is a comparison of two decoders on identical captures,
and that comparison is robust to the path. They are not a link budget.

**The band was not swept here.** The default 3000/6000 Hz pair was used
throughout. `mcl-ap` registry policy still says that pair must not be
standardised on the strength of one campaign, and this campaign does not change
that. `bootstrap_air --band <f0>:<f1>` and the firmware's `BAND` command exist
so the sweep can be run, and it has not been.

**Contention was not measured.** One transmitter, one receiver. Ten machines
answering one `PRESENCE` defeat a perfect modem, and nothing here says
otherwise.

**A host-loopback cell was attempted and is not a usable control.** This
laptop's speaker has a measured notch around 3 kHz (Experiment 002), which is
one of the two default FSK tones, and the loopback path recovered 1 of 3 at 10
bytes where the board rig recovers 9 of 15. It is recorded here as the reason
the shared-clock control does not exist, rather than omitted so that the missing
row has to be inferred.

> **Correction, 2026-09-07 — the measurement stands, the explanation does not.**
> The loopback result (1 of 3 at 10 bytes) is unchanged and is still the reason
> the shared-clock control does not exist. The attribution to "a measured notch
> around 3 kHz" is **withdrawn**. Experiment 012 measures 3000 Hz on this same
> host path at **65.92 dB SNR**, one of its stronger bins; the notch is at
> **3600 Hz** and is about one bin wide, touching neither tone. The 3 kHz figure
> was carried forward from Experiment 002 and was reinforced by a scoring defect
> that let a single bin decide a pair — see
> `experiments/012-multi-transmitter-band/README.md`. Two further facts now
> apply to this cell: the host speaker is **physically damaged**, with rising
> mechanical noise, so it is no longer a qualifying transmitter for band
> selection; and this row therefore stays published as **negative and context
> evidence**, which is what it was always good for. Nothing above this line has
> been altered.

## 6. Reproducing

```
bootstrap_air sizes                       # confirm 10 / 16 / 17 bytes
./run-bootstrap-air.ps1 -Rig board -Cell all -Trials 15 -Gain 75
```

`-Gain` scales the emitted samples on their way to the amplifier. It is a rig
control, not a protocol one: the waveform is unchanged and two implementations
at different volumes are both conforming. It is written into every run log,
because the signal-to-noise ratio is a property of the cell.
