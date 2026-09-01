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

## Impairment Harness

Deterministic impairments for controlled testing:

- **AWGN**: Configurable SNR (dB)
- **Clipping**: Configurable threshold (0.0–1.0)
- **Sample rate offset**: Configurable PPM offset
- **Deterministic RNG**: xorshift32 with configurable seed

## Results Disclaimer

All results from this experiment are **preliminary** and **do not define MCL-AP**.
The preamble and modulation choices here are **not frozen** into the protocol.
