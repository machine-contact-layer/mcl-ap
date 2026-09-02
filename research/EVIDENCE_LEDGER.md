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
| Real MCL waveform speaker->air->mic recovery | E3 | RECOVERED 8/10 | laptop speaker -> air -> DFR1154 ESP32-S3 PDM microphone -> USB CDC (board CRC-32 verified); 10 fresh trials against the frozen receiver gave 10/10 acquisition (corr 0.874086-0.877460), 10/10 PHY header, 8/10 exact Wire bytes and exact PRESENCE object; 3/3 pilot. Evidence under evidence/e3-dfr1154-20260902-frozen-rx/ | no AP-B0 selection, no multi-device (E4) claim, no range/SPL/robustness claim, no independent-implementation claim; single operator and single device pair only |
| Retained DFR1154 capture-005 exact decode | E2 | RECOVERED | decodes to exact Wire bytes, but only through a receiver changed after the capture was taken (symbol-boundary rounding) | not E3, and not evidence for the receiver change that enabled it |
| Same-laptop path frequency response | E3 preparation | measured, diagnostic only | both RAW microphone channels detected 500 Hz to 16 kHz stepped tones; 5 kHz was 15.4/21.7 dB below 3 kHz in the operator-reported Nahimic-disabled run | no calibrated SPL, no general device-response model, and no protocol success claim |
| DFR1154 PDM microphone FSK tone imbalance | E3 diagnostic | measured | 5 kHz mark tone approximately 12.4 dB below the 3 kHz space tone; large DC component (~1079 counts vs ~297 counts AC RMS); leaves ~0.3 log-energy decision margin on end-of-frame symbols and causes the 2/10 payload failures | no general transducer-response model; specific to this board |
| Laptop-speaker -> DFR1154 path frequency response | E2/E3 instrument (Exp 002 part A) | measured, 3 captures | stepped 22-tone probe, DC-removed Goertzel: 5 kHz measures 19-21 dB below 3 kHz inside a notch bottoming near 4200 Hz, while 5500/6000/8000/9000 Hz sit at or above the 3 kHz reference and 9 kHz is 10-12 dB stronger; reproducible across three independent playback offsets | one device pair, one direction, one geometry; cannot separate transmitter from receiver response; no AP-B0 selection and no claim about any other hardware |
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

1. Repeat E3 on a separated COTS speaker/microphone device pair (E4), without changing the fixed source toward the result. The 2026-09-02 result used one laptop speaker and one DFR1154 board.
1a. Raise FSK decision margin so recovery is not transducer-limited. Experiment 002 part A measured the cause directly: the 3/5 kHz pair straddles a notch, with 5 kHz 19-21 dB down while 6-9 kHz is at or above the 3 kHz reference. This is an AP-B0 candidate question. Experiment 001 stays frozen at 3/5 kHz; it is the instrument holding the 8/10 E3 result.
1b. Measure the reverse direction and a second device pair, so transmitter and receiver response can be separated. Requires either board-speaker transmit firmware (MAX98357 on BCLK 45 / LRCLK 46 / DIN 42, enable GPIO 3) or a second physical speaker/microphone.
2. E3 per-tile AP-B0 Pd/Pfa under controlled SNR/noise.
3. E4 repetition across multiple COTS device pairs.
4. E6 independent implementation only after Candidate Specification maturity.
