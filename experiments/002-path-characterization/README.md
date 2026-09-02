# MCL-AP Experiment 002: Real Transducer Path Characterization

**Status**: LAB / EXPERIMENTAL
**This is a measurement instrument, not a modem. It does NOT select AP-B0.**

## Purpose

Experiment 001 established that MCL Wire bytes survive a real acoustic path (E3, 8/10). It
did not establish *where in the spectrum* that path is actually usable. Experiment 002
measures the frequency response of a specific speaker → air → microphone pair so that band
and profile choices come from measurement rather than intuition.

The architecture has always held that there is no universal speaker/microphone response
curve, and that runtime path sounding must dominate advertised device specifications. This
is the first instrument that actually produces such a measurement.

## Method

A stepped-tone probe of equal amplitude and duration per tone, preceded by an LFM marker
chirp so the analyzer can locate the probe inside a capture with unknown playback latency.

| | |
|---|---|
| Tones | 22 tones, 300 Hz to 12 kHz |
| Tone duration | 90 ms, 10 ms gaps, raised-cosine edges |
| Marker | 200 ms LFM chirp, 2 kHz → 6 kHz |
| Amplitude | 0.45 full scale, identical for every tone |
| Format | 48 kHz, PCM16, mono, 2.610 s total |

Analysis removes the DC component first — PDM microphones carry a large one, and leaving it
in corrupts both the marker correlation and every level estimate — then measures each tone
with a Goertzel filter over the tone interior, excluding the edges.

```
mcl_ap_exp002_sweep gen  exp002_probe.wav
mcl_ap_exp002_sweep anal <capture>.wav
```

## Result: laptop Realtek speaker → DFR1154 PDM microphone, 2026-09-02

Three independent captures, taken at different playback offsets (marker found at samples
16128, 9452, 5306), all levels in dB relative to the 3 kHz tone:

| Tone (Hz) | cap-01 | cap-02 | cap-03 | Note |
|-----------|--------|--------|--------|------|
| 300   | −24.1 | | | |
| 500   | −7.2 | | | |
| 1000  | −5.6 | | | |
| 1300  | +4.5 | | | |
| 2000  | −1.9 | | | |
| 2800  | −1.0 | | | |
| **3000**  | **0.0** | **0.0** | **0.0** | reference — Experiment 001 FSK space tone |
| 3400  | −2.4 | | | |
| 3800  | −7.1 | | | |
| 4200  | −32.0 | −21.6 | −19.3 | deepest notch |
| 4600  | −16.4 | | | |
| **5000**  | **−21.1** | **−19.2** | **−19.1** | Experiment 001 FSK mark tone |
| 5500  | +0.3 | −1.6 | −2.4 | |
| 6000  | +0.8 | +1.3 | +1.5 | |
| 7000  | −1.4 | | | |
| 8000  | +8.2 | +10.1 | +10.5 | |
| 9000  | +10.4 | +11.9 | +12.5 | strongest measured |
| 10000 | +1.0 | | | |
| 12000 | −85.8 | | | effectively dead |

Full per-capture output is retained in `evidence/dfr1154-20260902/response-0N.txt`.

## What this means

The Experiment 001 FSK pair straddles a notch. The 3 kHz space tone sits in a strong region,
but the 5 kHz mark tone lands about **19–21 dB below it**, inside a broad depression that
bottoms out near 4200 Hz. That is a considerably worse imbalance than the ~12.4 dB estimated
indirectly from the training sequence during E3, and it explains the residual 2/10 payload
failures: the two FSK hypotheses are being compared across a 20 dB response step.

Meanwhile 5500, 6000, 8000 and 9000 Hz are all *at or above* the 3 kHz reference on this
path, with 9000 Hz measuring 10–12 dB stronger.

So the E3 result is not near a physical limit of this path. It was obtained across an
unlucky pair of frequencies, and a pair chosen from this measurement would have far more
decision margin.

## What this does NOT establish

- **AP-B0 is still NOT selected.** One device pair, one geometry, one room, one operator.
- This is a single direction (laptop speaker → board microphone). The reverse direction is
  unmeasured, and the architecture expects A→B and B→A to differ.
- The measurement cannot separate transmitter response from receiver response, because both
  are in series. Doing so needs a second, materially different device on one end.
- The 12 kHz result bounds the usable band on this path but says nothing about why — speaker
  rolloff, PDM decimation filtering, and room effects are all still confounded.
- No conclusion may be drawn here about any other hardware.

**Experiment 001 must not be retuned on the strength of this.** Its 3/5 kHz waveform is a
frozen evidence instrument holding the 8/10 E3 result; changing its frequencies would destroy
the value of that result. Better band choices belong to AP-B0 candidate work, evaluated on
their own evidence.
