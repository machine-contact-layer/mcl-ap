# MCL-AP Bootstrap and Link Convergence

Status: **Research Draft**

This document defines the working procedure by which unequal audio-capable machines converge onto a usable acoustic link.

## 1. Principle

The initiator does not assume the responder's preferred frequency or device response. It emits a compact, frequency-diverse discovery sequence. A responder only needs to recover a sufficient subset to identify MCL-AP and produce a response.

The effective capability is not merely the advertised speaker or microphone bandwidth. It is the measured directional path:

```text
A speaker → air → B microphone
```

at the current range, orientation, environment, noise, and processing state.

## 2. Discovery superframe

The initial research design uses time-frequency diversity rather than one fixed carrier.

Conceptual layout:

```text
time →

band E    ██          ██
band D        ██  ██
band C    ██      ██
band B        ██      ██
band A    ██      ██
```

Each discovery tile carries or contributes to a recoverable bootstrap identity.

Research goals:

- survive partial band loss
- keep per-tile energy reasonable
- avoid excessive peak-to-average ratio
- expose which frequency regions survived
- permit robust synchronization
- keep total acquisition time bounded

## 3. Bootstrap microframe

Working logical fields:

```text
BOOTSTRAP {
    protocol_magic
    ap_version
    frame_type
    ephemeral_node_id
    reply_window_or_slot
    capability_digest
    integrity_check
}
```

Initial target: approximately 8–20 bytes before FEC for the minimum bootstrap. This is an engineering hypothesis, not a frozen requirement.

## 4. Partial recovery

A responder need not decode every discovery tile.

A detector may produce:

```text
DetectionReport {
    ephemeral_node_id?
    observed_tile_mask
    observed_profile_hints
    band_quality[]
    timing_offset
    frequency_offset?
    confidence
}
```

A valid protocol identity may be reconstructed from a sufficient subset of tiles using redundancy/erasure-tolerant structure.

The exact minimum subset is to be selected experimentally.

## 5. Stage 0 — acquisition

The initiator transmits the discovery superframe.

The responder:

1. detects candidate preamble/tile energy
2. validates MCL-AP signature
3. estimates coarse timing
4. records surviving frequency regions
5. recovers bootstrap microframe if possible

## 6. Stage 1 — bootstrap response

The responder answers using the most robust viable response profile inferred from acquisition, not necessarily the fastest one.

Working response:

```text
CAPABILITY_RESPONSE {
    protocol_version
    supported_profile_classes
    sample_rate_class?
    duplex_mode
    maximum_frame_size
    preferred_reply_profile
    observed_forward_link_quality
    alternative_transports[]
}
```

Static device capability may be included, but measured path quality takes precedence for profile selection.

## 7. Stage 2 — directional sounding

Peers send known probes so each direction can be evaluated independently.

A probe may estimate:

- frequency availability/quality map
- SNR-like band quality
- frequency-selective attenuation
- multipath delay spread
- timing offset
- frequency offset
- sample-clock mismatch
- noise occupancy
- clipping/nonlinearity indicators

The probe waveform family is experimental. Candidate families include chirps, structured multitones, and pilot-rich multicarrier signals.

## 8. Stage 3 — profile selection

The negotiated state may be asymmetric:

```text
A → B: AP-R1 profile X, rate/coding X
B → A: AP-R1 profile Y, rate/coding Y
```

The logical MCL session remains bidirectional even if physical parameters differ.

Working selected state:

```text
DirectionalProfile {
    profile_id
    frequency_or_carrier_map
    symbol_duration
    modulation
    fec_mode
    repetition_policy
    ack_timing
    max_payload
    quality_target
}
```

## 9. Stage 4 — established traffic

MCL Link frames are carried using selected directional profiles.

MCL semantic priority may influence:

- coding strength
- repetition
- scheduling
- maximum retry budget
- field protection

without changing semantic meaning.

## 10. Stage 5 — continuous adaptation

The controller tracks link behavior and may:

- disable failing carriers
- move to another usable region
- increase symbol duration
- increase FEC
- lower bitrate
- repeat critical fields
- change acknowledgement timing
- fall back to AP-B0

No human reconfiguration should be required for ordinary adaptation.

## 11. Fallback

If the active profile degrades below its reliability target:

```text
ESTABLISHED
    ↓
ADAPT
    ↓ unsuccessful
FALLBACK
    ↓
AP-B0 acquisition / probe
```

If no acoustic profile remains viable, MCL Link may retain semantic state and use another binding if policy/capabilities allow.

## 12. Receive-only / transmit-only nodes

The profile architecture must allow partial compliance:

- receive-only contact node
- transmit-only broadcaster
- half-duplex node
- full-duplex node
- adaptive multi-profile node

A receive-only node may still decode Tier-0 hazards or requests. A transmit-only authority/safety device may still broadcast MCL contact semantics.

## 13. Conformance targets

Bootstrap conformance should eventually test:

- exact reference waveform generation
- detector probability at defined channel conditions
- false alarm rate
- partial-tile recovery
- frequency offset tolerance
- timing offset tolerance
- band erasure tolerance
- colored noise
- multipath
- sample-rate mismatch
- codec/recording-path damage where relevant

## 14. Selection criterion

The bootstrap is not selected by maximum raw bitrate.

Primary objective:

```text
maximize successful first-contact acquisition across heterogeneous paths
```

subject to:

- bounded acquisition time
- reasonable bandwidth
- reasonable compute
- robustness to audio hardware diversity
- low false-positive rate
- safe/controllable emission constraints

## 15. Open experiments

- serial sweep vs tiled superframe vs chirp-assisted acquisition
- audible low/mid-band vs higher-band diversity
- simultaneous versus time-staggered tiles
- erasure-coded bootstrap identity
- detector bank versus single adaptive detector
- one-channel versus multi-microphone combining
- fixed profile versus automatic convergence
