# E4 — Multi-device over-air, reverse direction

**Date**: 2026-09-02
**Evidence level**: E4 (multi-device over-air)
**Result**: 10/10 preamble acquisition, **8/10 exact Wire bytes and exact PRESENCE recovery**

The first MCL frame recovered over air across a **different device pair in the opposite
direction** from E3.

## Why this is E4 and not another E3 run

E3 used the laptop Realtek speaker as transmitter and the DFR1154 PDM microphone as receiver.
This set reverses both ends:

| | E3 (forward) | This set (reverse) |
|---|---|---|
| Transmitter | laptop Realtek speaker | **DFR1154 MAX98357 amplifier + onboard speaker** |
| Receiver | DFR1154 PDM microphone | **laptop Realtek microphone array** |
| Direction | A → B | **B → A** |

Neither transducer is shared with E3. The transmit and receive clocks belong to different
devices, and the emitting device is now the embedded one.

The architecture has always held that A→B and B→A are different channels and must be measured
separately. This is the first measurement of the return path.

## Conditions

| | |
|---|---|
| Waveform | Experiment 003 candidate, FSK 3000/6000 Hz, 300 baud |
| Source | `../../exp003_source.wav`, SHA-256 `7192A2BE…14CAD`, embedded in flash as `exp003_pcm.h` |
| Transmitter | DFR1154 firmware v2, `PLAY` command, MAX98357 on BCLK 45 / LRCLK 46 / DIN 42 |
| Firmware image | `dfr1154_usb_capture.ino.bin`, SHA-256 `B3DA5E3FFD99D1B36AEA845F308E27A3C7B0DB062B84E5CE9D592BF5F3CBBB88`, flashed to the app partition at `0x20000` only |
| Receiver | laptop Realtek microphone array via ffmpeg dshow, 48 kHz PCM16 mono |
| Audio enhancements | Off |
| Expected payload | `00 02 00 00 00 01 01 00 00 01 3C` — Tier-0 PRESENCE |

## Results

| Trial | Acquired | Correlation | Exact Wire + semantic |
|-------|----------|-------------|-----------------------|
| 1  | yes | 0.883488 | yes |
| 2  | yes | 0.883833 | yes |
| 3  | yes | 0.883288 | yes |
| 4  | yes | 0.885269 | no  |
| 5  | yes | 0.888696 | no  |
| 6  | yes | 0.883801 | yes |
| 7  | yes | 0.883121 | yes |
| 8  | yes | 0.883373 | yes |
| 9  | yes | 0.885446 | yes |
| 10 | yes | 0.887736 | yes |

Acquisition **10/10**, exact recovery **8/10**. A 3-trial pilot immediately before this set
acquired 3/3 and recovered 1/3.

## What this shows

Acquisition on this reverse path is markedly stronger than on the laptop-speaker →
laptop-microphone path measured the same day (0.883–0.889 here against 0.35–0.53 there), even
though the receiver is the same microphone in both. The variable that changed is the
transmitter.

That is consistent with the Experiment 002 finding that the notch is in the laptop speaker
rather than in either microphone: replacing that speaker with the board's amplifier removes
the weak link, and the same microphone that recovered 0/10 from the laptop speaker recovers
8/10 from the board.

## What this does NOT establish

- **AP-B0 is still NOT selected.** The 3000/6000 pair was chosen from a measurement of the
  *forward* path. This reverse path has not been swept, and its optimum may differ — that
  asymmetry is exactly what the architecture predicts and has not yet been measured.
- Two physical devices in reversed roles is a genuine second pairing, but it is not a third
  independent device, and it is still one operator, one room, one geometry, one session.
- 8/10 is not a usable link. No range, motion, multipath or multi-node claim follows.

## Reproduction

Flash firmware v2 to the app partition only, then:

```
# host records first, board guards 500 ms after ARMED
ffmpeg -f dshow -i audio="Microphone Array (Realtek(R) Audio)" -t 4 -ar 48000 -ac 1 -c:a pcm_s16le trial.wav
# send PLAY on COM3 at 921600
mcl_ap_exp003 decode trial.wav
```

The board must be restored afterwards with `RESTORE_DFR1154_APP.cmd`.
