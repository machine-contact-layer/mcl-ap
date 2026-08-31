# Foundational Acoustic Calculations

These utilities deliberately separate deterministic propagation calculations from device-specific transmitter/microphone assumptions.

## Implemented

- ISO 9613-1 atmospheric absorption
- temperature-dependent sound speed
- free-field geometric spreading relative to 1 m
- propagation delay
- generated frequency × distance envelope

The 20 °C / 50% RH sanity values reproduce the standard-derived reference implementation near:

```text
1 kHz   4.66 dB/km
2 kHz   9.89 dB/km
4 kHz  29.67 dB/km
8 kHz 105.29 dB/km
```

## Important boundary

The result is **not an end-to-end link budget**. Speaker output, microphone sensitivity/noise, orientation, enclosure, beamforming, ground effects, obstruction, room response, AGC/AEC, and environmental noise must be added separately or measured.

## Immediate consequence

At 20 °C / 50% RH, 20 kHz accumulates about 25.7 dB of atmospheric loss between 1 m and 50 m, before geometric spreading. At 4 kHz the corresponding atmospheric term is about 1.45 dB. This makes a fixed high-frequency profile structurally unsuitable as the only MCL-AP mode.
