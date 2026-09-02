# E3 — Controlled Over-Air MCL Frame (DFR1154, frozen receiver)

**Date**: 2026-09-02
**Evidence level**: E3 (controlled over-air)
**Result**: 8 / 10 trials recovered the exact MCL Wire bytes and the exact PRESENCE semantic object.

This is the first MCL frame to survive a real speaker → air → microphone path.

## Why this set is E3 and the earlier one is not

The retained `e3-dfr1154-20260902/capture-005-crc-verified.wav` also decodes exactly, but
only through a receiver that was changed *after* that capture was taken (symbol boundaries
are now rounded to nearest rather than truncated). Under the evidence rules that makes it
recorded-replay evidence, E2, not E3.

These ten captures were taken **after** that receiver change, against the frozen decoder,
and were decoded without any further modification. Nothing in the transmit waveform,
decoder, detection threshold, modulation, or gain was adjusted between trials.

## Conditions

| | |
|---|---|
| Source | `exp001_e3_source.wav`, SHA-256 `1BCA567F7F68F5A18F41ADD8CDE03863C336D50F332097F8361262F56ECA1241` |
| Transmit path | laptop default speaker (Realtek), `System.Media.SoundPlayer` |
| Receive path | DFR1154 ESP32-S3, onboard PDM microphone (GPIO 38 clock / 39 data) |
| Capture format | 48 kHz, PCM16, mono, 3.000 s |
| Transport | native USB CDC on COM3, board-side IEEE CRC-32 verified host-side before the WAV is written |
| Detection threshold | 0.40 (frozen) |
| Modulation | binary FSK, 3000 Hz / 5000 Hz, 300 baud (LAB/EXPERIMENTAL, **not** AP-B0) |
| Expected payload | `00 02 00 00 00 01 01 00 00 01 3C` — Tier-0 PRESENCE |

## Results

| Trial | Preamble acquired | Correlation | CRC valid | Wire byte-exact | Semantic exact |
|-------|-------------------|-------------|-----------|-----------------|----------------|
| 1  | yes | 0.876386 | yes | yes | yes |
| 2  | yes | 0.877460 | yes | yes | yes |
| 3  | yes | 0.875203 | yes | yes | yes |
| 4  | yes | 0.876401 | yes | yes | yes |
| 5  | yes | 0.874439 | no  | no  | no  |
| 6  | yes | 0.876328 | yes | yes | yes |
| 7  | yes | 0.874238 | yes | yes | yes |
| 8  | yes | 0.876102 | yes | yes | yes |
| 9  | yes | 0.874086 | no  | no  | no  |
| 10 | yes | 0.876144 | yes | yes | yes |

Preamble acquisition: **10 / 10**. PHY header recovery: **10 / 10**. Payload recovery: **8 / 10**.

`trials.csv` is the machine-readable form. Note that its `Acquired` column reports `no` for
trials 5 and 9; that column is derived from the decoder's success-path output and is wrong
for those rows. Both trials did acquire, at correlations 0.874439 and 0.874086.

## Characterization of the two failures

Both failures are identical in shape and neither is an acquisition failure:

```
trial 05: start=13531 correlation=0.874439  header len=11 crc=0x219A (correct)  payload CRC 0xFE30
trial 09: start= 5514 correlation=0.874086  header len=11 crc=0x219A (correct)  payload CRC 0xB012
```

In both, the preamble acquires cleanly and all 24 PHY header bits decode correctly, including
the transmitted CRC field. Only payload symbols are misdecided.

The measured mechanism is the transducer response, not timing: the DFR1154 PDM microphone
attenuates the 5 kHz mark tone by roughly 12.4 dB relative to the 3 kHz space tone, and adds a
large DC component (~1079 counts against ~297 counts AC RMS). The receiver compensates with a
training-derived log-energy decision bias, but the two FSK hypotheses remain close enough that
the weakest symbols — consistently near the end of the frame — sit within about 0.3 of the
decision boundary.

Raising that margin is the next AP engineering problem. It is a modulation and band-selection
question, and it is exactly the kind of question AP-B0 selection is supposed to answer with
evidence. **AP-B0 remains NOT selected.** Nothing in this directory freezes any waveform
parameter into MCL-AP.

## Reproduction

```
powershell -File ../../hardware/dfr1154-usb-capture/capture-e3.ps1 -OutputPath <new>.wav
mcl_ap_exp001_decode_wav.exe <new>.wav
```

The capture script refuses to overwrite an existing artifact and always restores the laptop
audio configuration on exit.
