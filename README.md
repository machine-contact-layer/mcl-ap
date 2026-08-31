# MCL-AP

**MCL-AP** is the Acoustic Profile of the Machine Contact Layer.

It provides a reference software-defined path for first contact and MCL communication through audio I/O, while allowing OEMs to extend or replace the physical implementation without changing MCL Core semantics or MCL Link behavior.

## Role in MCL

```text
MCL Core semantics
        ↓
MCL Wire
        ↓
MCL Link
        ↓
MCL-AP
bootstrap / sounding / link convergence / PHY / FEC
        ↓
Audio I/O
        ↓
OEM speaker / microphone / arrays / DSP
```

## AP owns

- acoustic discovery/bootstrap
- frequency-diverse acquisition
- synchronization
- end-to-end channel sounding
- directional link estimation
- profile selection
- adaptation/fallback
- reference modulation schemes
- FEC integration
- acoustic framing integration with MCL Link
- channel models and replay harnesses
- acoustic conformance vectors and experiments

## AP does not own

- speaker or microphone design
- amplifier design
- enclosure design
- beamforming hardware
- array geometry
- machine chassis
- application-specific range
- OEM emission power targets

## Foundational assumption

MCL-AP does not require matched transducers. A compliant implementation provides at least one bootstrap path. The link-convergence engine measures the actual directional path and selects a mutually usable profile.

Forward and reverse directions may use different physical profiles while remaining one logical MCL session.

## Working profile families

- **AP-B0** — mandatory/bootstrap research profile
- **AP-R1** — robust governing/contact traffic
- **AP-W2** — wider-band higher-rate traffic when available
- **AP-X** — extension/OEM/experimental profiles

Exact frequencies, modulation, symbol duration, and FEC parameters are **not frozen yet**. They must fall out of reproducible experiments across heterogeneous devices and channels.

## Research priorities

1. Maximize bootstrap acquisition across heterogeneous audio paths.
2. Minimize time-to-contact and time-to-profile-convergence.
3. Measure usable semantic events, not only raw bitrate.
4. Compare fixed PHYs against adaptive link convergence.
5. Compare generic-byte protection against semantic/priority-aware protection under the same channel budget.
6. Retain source waveforms, received captures, channel parameters, and test vectors for reproducibility.

## Documents

- [`spec/ap-v0.md`](spec/ap-v0.md) — profile architecture
- [`spec/bootstrap-link-convergence.md`](spec/bootstrap-link-convergence.md) — bootstrap and convergence procedure
- [`experiments/README.md`](experiments/README.md) — experimental program

## Status

Private research repository. Pre-v0.1 candidate specification. Not an adopted standard.
