# MCL-AP Experiment 003: Band-Informed FSK Candidate

**Status**: LAB / EXPERIMENTAL — AP-B0 **candidate under evaluation**, not a selected profile.

## What this changes, and what it deliberately does not

Experiment 001 picked 3000/5000 Hz before any real acoustic path had been measured.
Experiment 002 part A then measured the path and found the 5 kHz mark tone sitting 19–21 dB
down inside a notch, while 6 kHz sits at or above the 3 kHz reference on both receivers
available here.

This experiment changes **exactly one variable**: the mark tone moves from 5000 Hz to 6000 Hz.

Held identical to Experiment 001: the Tier-0 PRESENCE object and its canonical Wire bytes, the
frame layout, the LFM 2–6 kHz preamble, the training sequence, 300 baud, 48 kHz PCM16 mono, the
receiver, and the 0.40 detection threshold. Because only the band differs, any difference in
recovery is attributable to band choice rather than to a new modem.

| | Experiment 001 | Experiment 003 |
|---|---|---|
| Space tone (bit 0) | 3000 Hz | 3000 Hz |
| Mark tone (bit 1) | 5000 Hz | **6000 Hz** |
| Everything else | — | identical |

Source: `exp003_source.wav`, SHA-256 `7192A2BEF785540280B6137C69DDB7E7E8828AB9D8FECC1CCB4C115626614CAD`.
Payload: `00 02 00 00 00 01 01 00 00 01 3C`.

## Result, 2026-09-02

All four sets below were captured in a single session on one physical setup, so they are a
controlled comparison rather than a comparison across time. This matters: conditions had
degraded since the original E3 run (preamble correlation about 0.56 on the board, against
0.874 during E3), so these numbers describe a *harder* channel than E3 saw.

| Receiver | Waveform | Preamble acquired | Exact Wire + semantic recovery |
|----------|----------|-------------------|-------------------------------|
| DFR1154 PDM | Exp 001 — 3000/5000 | 10/10 | **0/10** |
| DFR1154 PDM | Exp 003 — 3000/6000 | 9/10 | **9/10** |
| Laptop Realtek array | Exp 001 — 3000/5000 | 8/10 | **0/10** |
| Laptop Realtek array | Exp 003 — 3000/6000 | 10/10 | **4/10** |

On both receivers the measured band choice takes recovery from total failure to working, on a
channel where the original waveform no longer recovers anything at all.

### Reverse direction (E4)

The board also transmits. Firmware v2 adds a `PLAY` command that emits this candidate frame
through the DFR1154 MAX98357 amplifier, so the return path can be measured:

| Path | Acquired | Exact recovery |
|------|----------|----------------|
| board speaker → laptop Realtek mic | 10/10 @ 0.883–0.889 | **8/10** |

That is the first MCL frame recovered across a different device pair in the opposite
direction, and it separates transmitter from receiver response: the same laptop microphone
recovers 0/10 from the laptop speaker and 8/10 from the board speaker. The weak link was the
laptop speaker, exactly as Experiment 002 indicated.

Evidence:

- `evidence/e4-board-speaker-to-laptop-mic-20260902/` — reverse-direction E4
- `evidence/dfr1154-20260902/` — candidate on the board receiver
- `evidence/laptop-realtek-20260902/` — candidate on the laptop receiver
- `evidence/control-exp001-baseline-dfr1154-20260902/` — the 3000/5000 control captured in
  the same session on the board, which is what makes the comparison valid

## Reading this honestly

The headline is **not** "3000/6000 Hz is the right AP-B0 waveform." It is:

1. Band choice, measured on the actual path, dominates recovery for this class of waveform.
   That is a result about method, and it is the one the architecture predicted — MCL-AP is a
   path-convergence system, and profiles should be selected from measurement rather than
   assumed.
2. Binary FSK at a fixed pair remains fragile. 4/10 on the laptop receiver is not a usable
   link. A real profile needs either frequency diversity, coding, or per-path selection —
   which is precisely what Experiment 002 part B and AP-B0 selection exist to determine.

## What this does NOT establish

- **AP-B0 is NOT selected.** One transmitter, two receivers, one room, one geometry, one
  operator, one session.
- 3000/6000 Hz is tuned to *this measured path*. It has no general validity, and the
  architecture explicitly forbids freezing spectrum on this kind of evidence.
- No claim about range, motion, multipath, multi-node contention, or any other hardware.
- Experiment 001 remains frozen at 3000/5000 Hz and must not be retuned. It is the instrument
  holding the 8/10 E3 result, and its value depends on not moving.

## Reproduction

```
mcl_ap_exp003 gen    exp003_source.wav
mcl_ap_exp003 decode <capture>.wav
```

`decode` acquires across the whole capture before handing the frozen decoder a window
positioned the way it expects, because a host recording has arbitrary lead-in. That changes
where the receiver is pointed, never how it works.
