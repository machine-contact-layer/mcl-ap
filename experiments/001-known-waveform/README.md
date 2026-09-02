# MCL-AP Experiment 001: Known-Waveform Acoustic Modem

**Status**: LAB / EXPERIMENTAL  
**This is NOT a normative AP profile. This is NOT AP-B0.**

## Purpose

First bit-perfect acoustic encode/decode path using real MCL Wire-encoded bytes.

## Architecture

```
MCL SDK (Tier-0 object)
    |
    v
MCL Wire codec  -->  canonical bytes
    |
    v
Experiment 001 frame encoder:
    [preamble] [PHY header: len + CRC-16] [binary FSK payload] [silence]
    |
    v
PCM WAV (48 kHz, 16-bit, mono)
    |
    v  (speaker -> air -> microphone for E3)
    |
    v
Experiment 001 frame decoder:
    cross-correlation preamble detection
    Goertzel FSK demodulation
    CRC-16 verification
    |
    v
recovered canonical bytes
    |
    v
MCL Wire decode  -->  Tier-0 object (must match source exactly)
```

## Modulation

- **Type**: Binary FSK (deliberately simple and robust)
- **Frequencies**: f0 = 3000 Hz (bit 0), f1 = 5000 Hz (bit 1)
- **Baud rate**: 300 baud (= 300 bps for binary FSK)
- **Sample rate**: 48000 Hz mono PCM

## Preamble Candidates

Four candidate preambles are implemented for the bakeoff:

1. **LFM Chirp** — Linear frequency modulated sweep
2. **Zadoff-Chu** — ZC sequence modulated onto carrier (N=127, u=7)
3. **PN/m-sequence** — 7-bit LFSR (period 127) BPSK modulated
4. **Frequency-diverse** — 4-tone repeated pattern

## Files

| File | Purpose |
|------|---------|
| `exp001.h` | Public API: preamble, FSK, CRC, WAV, frame, impairments |
| `exp001.c` | Core implementation |
| `test_exp001.c` | Unit tests + full pipeline + E3 WAV generator |
| `bakeoff.c` | Preamble comparison under controlled impairments |

## Build (MSVC)

```
cl /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS ^
   /I../../include /I../../../mcl-wire/include ^
   exp001.c test_exp001.c ^
   ../../../mcl-wire/src/wire.c ../../../mcl-wire/src/extension.c ^
   /Fe:test_exp001.exe

cl /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS ^
   /I../../include /I../../../mcl-wire/include ^
   exp001.c bakeoff.c ^
   ../../../mcl-wire/src/wire.c ../../../mcl-wire/src/extension.c ^
   /Fe:bakeoff.exe
```

## E3 Path

1. Run `test_exp001.exe` — generates `exp001_e3_source.wav`
2. Play the WAV through a speaker
3. Record microphone capture as 48 kHz 16-bit mono WAV
4. Feed captured WAV to the offline decoder
5. If recovered Wire bytes match source exactly: **E3 achieved**

### E3 status: ACHIEVED 2026-09-02 (8/10)

The first over-air recovery of an exact MCL frame was obtained on 2026-09-02 using the
laptop speaker as transmitter and the DFR1154 ESP32-S3 onboard PDM microphone as receiver.
Ten trials against the frozen receiver gave 10/10 preamble acquisition, 10/10 PHY header
recovery, and **8/10 exact Wire bytes plus exact PRESENCE semantic object**; a 3-trial pilot
under identical conditions gave 3/3. Evidence, per-trial results, and SHA-256 manifests:
`evidence/e3-dfr1154-20260902-frozen-rx/` and `-frozen-rx-pilot/`.

Both failures acquired cleanly and decoded every PHY header bit correctly, failing only on
payload symbols. The measured cause is transducer response: the PDM microphone attenuates
the 5 kHz mark tone by roughly 12.4 dB relative to the 3 kHz space tone, leaving about 0.3
of log-energy decision margin on end-of-frame symbols. Raising that margin is the next AP
problem and is exactly what AP-B0 selection must answer with evidence.

Earlier negative results are retained and must not be reported as E3 success: the
same-laptop Realtek speaker/microphone attempts under `evidence/e3-laptop-20260902/`, the
follow-up frequency-response measurement and five fixed-source captures (including
Nahimic-disabled and microphone-boosted conditions) under
`evidence/calibration-followup-20260902/`, and the earlier DFR1154 captures under
`evidence/e3-dfr1154-20260902/`. The last of those does now decode exactly, but only through
a receiver changed after it was recorded, which makes it E2 replay evidence rather than E3.

## Impairment Harness

Deterministic impairments for controlled testing:

- **AWGN**: Configurable SNR (dB)
- **Clipping**: Configurable threshold (0.0–1.0)
- **Sample rate offset**: Configurable PPM offset
- **Deterministic RNG**: xorshift32 with configurable seed

## Results Disclaimer

All results from this experiment are **preliminary** and **do not define MCL-AP**.
The preamble and modulation choices here are **not frozen** into the protocol.
