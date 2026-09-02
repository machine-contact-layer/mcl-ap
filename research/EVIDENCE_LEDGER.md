# MCL-AP Evidence Ledger

Status: **Living Research Ledger**

This ledger separates architectural claims from evidence maturity. It is intentionally conservative.

Evidence labels follow the MCL conformance model:

- E0 ANALYTICAL
- E1 DETERMINISTIC_SIMULATION
- E2 RECORDED_CHANNEL_REPLAY
- E3 CONTROLLED_OVER_AIR
- E4 MULTI_DEVICE_OVER_AIR
- E5 OPERATIONAL_ENVIRONMENT
- E6 INDEPENDENT_INTEROPERABILITY

## Current evidence

| Claim / artifact | Level | Status | What it does establish | What it does not establish |
|---|---|---|---|---|
| Sound propagation delay / serialization crossover | E0 | established calculation | physical latency floor | real decoder/acquisition latency |
| ISO 9613-1 atmospheric absorption implementation | E0 | implemented | frequency/climate-dependent atmospheric loss model | complete device/channel SNR |
| Acoustic Doppler scaling calculations | E0 | established calculation | mobility produces large time/frequency scaling | performance of a specific receiver |
| Synthetic known-channel estimator validation | E1 | research result | estimator can recover a known synthetic channel under tested conditions | real speaker-air-mic accuracy |
| Five-band erasure-mask topology | E1 structural | combinatorial study only | one-of-N copied identity is structurally tolerant to band erasure | physical detection probability or 100% acquisition |
| Failed-TTS / MP3 adverse capture characterization | E2-adjacent | diagnostic only | real audio paths/codecs can strongly distort/suppress signals | MCL frame decode or channel capacity |
| Real MCL waveform speaker->air->mic recovery | E3 | RUN — NOT RECOVERED | same-laptop RAW capture preserved the chirp and one channel acquired at correlation 0.624806; later fixed-source repeats did not reacquire | no valid frame CRC, exact Wire recovery, semantic recovery, or over-air MCL success claim |
| Same-laptop path frequency response | E3 preparation | measured, diagnostic only | both RAW microphone channels detected 500 Hz to 16 kHz stepped tones; 5 kHz was 15.4/21.7 dB below 3 kHz in the operator-reported Nahimic-disabled run | no calibrated SPL, no general device-response model, and no protocol success claim |
| AP-B0 per-tile Pd/Pfa curves | E3/E4 target | NOT RUN | — | no physical acquisition probability yet |
| Semantic UEP vs uniform FEC advantage | E1/E3 target | NOT RUN | — | no +74.3% or similar survival claim is valid yet |
| Multi-node propagation-aware MAC | E1 target | NOT RUN | — | no collision-resolution performance claim yet |
| Independent MCL-AP implementation interoperability | E6 | NOT RUN | — | no standards-level interoperability claim yet |

## Publication rule

Any document quoting an MCL-AP result SHOULD include its evidence level.

A higher-level document MUST NOT convert:
- E0 into a measured device result;
- E1 into over-air evidence;
- E2 into known-source channel evidence when the transmitted source is unknown;
- a `NOT RUN` hypothesis table into measured recovery percentages.

## Immediate promotion targets

1. Repeat E3 first bit-perfect MCL frame on a separated COTS speaker/microphone device pair without changing the fixed source toward the result.
2. E3 per-tile AP-B0 Pd/Pfa under controlled SNR/noise.
3. E4 repetition across multiple COTS device pairs.
4. E6 independent implementation only after Candidate Specification maturity.
