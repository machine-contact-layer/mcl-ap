# MCL-AP v0

Status: **Research Draft**

## 1. Purpose

MCL-AP defines an acoustic transport profile for the Machine Contact Layer.

The profile is intended for physically co-present machines that may not share an RF network, vendor ecosystem, credentials, or prior relationship. It uses available audio I/O as a reference physical surface and may coexist with or hand off to BLE, UWB, IP, or another transport.

## 2. Architectural principles

### 2.1 Transducer-agnostic protocol

MCL-AP is specified above the physical speaker/microphone implementation. OEMs may use ordinary speakers, MEMS devices, arrays, directional systems, ultrasonic transducers, or future hardware.

### 2.2 No matched-pair assumption

The forward and reverse paths are different systems:

```text
A speaker → air → B microphone
B speaker → air → A microphone
```

MCL-AP therefore allows asymmetric directional profiles.

### 2.3 Bootstrap before optimization

A node first acquires a conservative contact signal, then measures the path, then selects a better physical profile if one exists.

### 2.4 Semantic invariance

Changing carrier allocation, FEC, modulation, bitrate, or acoustic profile MUST NOT change the meaning of MCL Core objects.

### 2.5 Local policy sovereignty

Acoustic reception establishes communication, not authority. Claims and requests remain subject to local validation/policy.

## 3. Working profile families

### AP-B0 — Bootstrap

Purpose:

- presence/contact acquisition
- protocol/version indication
- minimal capability response
- fallback

Desired properties:

- broad device compatibility
- frequency diversity
- strong synchronization
- small bounded microframe
- strong error detection/protection
- subset/partial-tile recovery where practical

### AP-R1 — Robust contact/governing traffic

Purpose:

- hazard
- compact request
- acknowledgement
- authority/identity references
- transport offers
- short state updates

Selected after initial path measurement.

### AP-W2 — Wideband contact traffic

Purpose:

- richer state
- trajectory
- sustained acoustic coordination
- higher-rate experimental traffic

Available only when the measured path supports it.

### AP-X — Extension

For OEM or experimental physical profiles, including:

- high ultrasonic bands
- directional arrays
- multichannel spatial signaling
- custom modulation
- learned physical-layer experiments

Extensions must still carry MCL Link/Wire semantics correctly.

## 4. AP interface to MCL Link

MCL Link provides:

- logical frame class
- source/destination/session references
- wire payload
- priority
- acknowledgement requirements
- freshness/trust hooks

MCL-AP provides:

- discovery indication
- directional link state
- selected profile IDs
- estimated quality
- transmit/receive service
- adaptation events
- fallback indication

Working interface:

```text
AcousticController {
    start_discovery()
    stop_discovery()
    probe(peer)
    supported_profiles()
    directional_state(peer)
    select_profile(peer, direction, profile)
    send(link_frame)
    on_frame(handler)
    on_link_event(handler)
}
```

## 5. Link-convergence state

```text
SILENT
  ↓ detect/send bootstrap
ACQUIRED
  ↓ validate contact microframe
PROBING
  ↓ estimate directional path
CONFIGURING
  ↓ select profiles
ESTABLISHED
  ↕ continuous adaptation
FALLBACK
  ↓
ACQUIRED / PROBING
```

## 6. Physical measurements

A channel probe may estimate:

- detectable frequency regions
- per-band SNR or quality score
- frequency-selective attenuation
- multipath delay spread
- timing offset
- frequency offset
- sample-clock mismatch
- clipping/nonlinearity indicators
- noise occupancy
- directional asymmetry

Absolute SPL/power is not required for basic interoperability but may be supplied by calibrated OEM implementations.

## 7. Physical-layer candidate family

The reference implementation should benchmark, not prematurely standardize:

- MFSK-like conservative signaling
- chirp/spread-spectrum signaling
- multicarrier/OFDM-like signaling
- hybrid/adaptive methods
- unequal error protection

The mandatory bootstrap waveform is a research result to be selected through cross-device experiments, not intuition.

## 8. Adaptation

A profile may adapt:

- active frequency region
- carrier set
- symbol duration
- coding rate
- repetition
- acknowledgement policy
- maximum payload
- transmit scheduling

Adaptation must preserve session and semantic meaning.

## 9. Channel model

Research simulation should include:

- propagation delay
- geometric spreading
- atmospheric absorption
- device response
- colored/non-Gaussian noise
- multipath
- motion and Doppler
- sample-clock error
- clipping and AGC-like distortion
- band erasures

Analytical models must be complemented by retained real captures.

## 10. Metrics

Required metrics include:

- acquisition probability
- time to acquisition
- time to profile convergence
- BER/PER where known bits are used
- verified goodput
- mandatory-field recovery
- semantic/task recovery
- spectral efficiency
- latency
- energy where calibrated
- resynchronization overhead
- performance across hardware variation
- performance under motion/noise/multipath

## 11. Non-goals

MCL-AP does not prescribe:

- one universal speaker
- one microphone
- one power level
- one range
- one frequency band for all links
- human-audible versus inaudible operation for every deployment
- OEM beamforming or array design

## 12. Research questions

1. What bootstrap waveform maximizes common acquisition across heterogeneous audio devices?
2. How quickly can directional paths converge to usable profiles?
3. How much does adaptation outperform a fixed modem profile?
4. How much mandatory machine meaning survives under equal physical resource budgets with semantic-aware protection?
5. Which effects dominate practical failure: bandwidth, device response, multipath, Doppler, noise, audio processing, or synchronization?
