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

## Experiment 002 — frequency-diverse bootstrap

Compare:

- fixed single-band bootstrap
- sequential frequency sweep
- time-frequency tiled superframe
- chirp-assisted acquisition
- erasure-tolerant tile identity

Metrics:

- acquisition probability
- false alarm rate
- time to acquisition
- band-erasure tolerance
- SNR/noise sensitivity
- device-pair coverage

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
