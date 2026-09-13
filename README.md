<p align="center">
  <img src="https://raw.githubusercontent.com/machine-contact-layer/.github/main/profile/banner.png" alt="Machine Contact Layer (MCL) banner: black and white checkerboard with the OJOBIT wordmark" width="100%">
</p>

<h1 align="center">MCL-AP</h1>

<p align="center"><strong>First contact through the air, using the speaker and microphone the machine already has.</strong></p>

<p align="center">
  Acoustic transport binding for the Machine Contact Layer (MCL): an audio modem
  and bootstrap profile that lets machines with no shared network discover each
  other over sound and agree on a better link such as BLE or Wi-Fi.
  Freestanding C99.
</p>

<p align="center">
  <a href="https://github.com/machine-contact-layer/mcl-ap/actions/workflows/ci.yml"><img alt="CI status" src="https://github.com/machine-contact-layer/mcl-ap/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/machine-contact-layer/mcl-ap/blob/main/LICENSE"><img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue"></a>
  <img alt="AP-BOOTSTRAP-1: Candidate" src="https://img.shields.io/badge/AP--BOOTSTRAP--1-Candidate-yellow">
  <img alt="Language: freestanding C99" src="https://img.shields.io/badge/C99-freestanding-informational">
</p>

<p align="center">
  <a href="https://github.com/machine-contact-layer/mcl-sdk"><b>SDK</b></a> ·
  <a href="https://github.com/machine-contact-layer/mcl-core"><b>MCL overview</b></a> ·
  <a href="spec/ap-bootstrap-1.md"><b>Specification</b></a> ·
  <a href="experiments/"><b>Experiments</b></a> ·
  <a href="https://github.com/machine-contact-layer/mcl-core/blob/main/REPORTING.md"><b>Report a defect</b></a>
</p>

---

Two machines with no shared network still share the air. MCL-AP is the binding
that needs no infrastructure at all — no access point, no pairing, no account —
just a speaker on one side and a microphone on the other, long enough to agree
on somewhere better to talk.

It is the bootstrap medium for `MCL Stranger-Contact 1` in the
[Machine Contact Layer](https://github.com/machine-contact-layer/mcl-core), and
it runs on embedded hardware: the Wire, Link and acoustic modem stack decodes
frames over the air on an ESP32-S3 with no host in the loop.

> **Building a product?** Start with [**mcl-sdk**](https://github.com/machine-contact-layer/mcl-sdk).
> Its reference deployment uses MCL-AP for first contact and then migrates to
> BLE. Come here for the waveform, the receiver and the conformance vectors.

## What MCL-AP provides

- **`AP-BOOTSTRAP-1`** — a fully specified acoustic bootstrap profile: binary
  FSK at 300 baud with continuous phase, preamble, framing and refusal rules
- **A reference modem** — modulator, preamble correlator, demodulator and frame
  check, in freestanding C99
- **A continuous listener** — acquisition driven by whatever audio blocks the
  capture path produces, so a machine hears a call it was not scheduled to
  expect
- **Conformance vectors as audio** — WAV files for valid objects and for each
  refusal case, pinned by digest
- **Experiments with retained captures** — over-air runs on laptops, an
  ESP32-S3 and an Android handset, including the runs that failed

## Where it fits

```text
MCL Core semantics
        ↓
MCL Wire  →  MCL Link
        ↓
MCL-AP     bootstrap · acquisition · modulation · framing
        ↓
audio I/O  your speaker, microphone, DSP
```

MCL-AP owns acoustic discovery, synchronization, modulation, framing and the
acoustic conformance vectors. It does not own speaker, microphone, amplifier or
enclosure design, array geometry, or emission power targets; OEMs can extend or
replace the physical implementation without changing MCL semantics or Link
behaviour. It does not require matched transducers.

## Operational limits

Acoustic is a rendezvous medium, not a data link. Plan around it:

- **It is slow.** At 300 baud a 17-byte `TRANSPORT_OFFER` occupies about 786 ms
  of air, which is why a shared medium needs contention handling and why a
  contact should migrate to BLE or IP once the machines have met.
- **Recovery depends on the device and payload length.** Acquisition on the
  archived over-air captures is reliable; full-frame recovery ranged from 2/10
  to 9/10 across transmitters and payload sizes.
- **Hearing a call is only reception.** The listener does not decide who sent a
  frame, whether its content is true, or that the machine owes a reply.

## Continuous listening

[`include/mcl/ap_listen.h`](include/mcl/ap_listen.h) is the receiver for a
machine doing its actual job: audio is pushed in whatever blocks the capture
path produces, and the machine polls whenever it gets a moment.

- **Acquisition is incremental.** Every sample is correlated against the
  preamble once, when it first becomes searchable — never once per poll — so a
  listener costs the same whether its window holds two seconds or sixty.
  `samples_searched` reports that directly.
- **The window is scheduling slack, not memory.** Size it by the longest stretch
  the application can go without polling.
- **Audio dropped unheard is counted.** `samples_unscanned` reports it, because a
  missed call and a quiet room are otherwise the same silence.

Streaming the archived over-air captures through the listener in blocks from 256
to 8192 samples reproduces the one-shot decoder exactly. Try it:

```text
mcl_ap_node listen-wire  <capture.wav> [block]
mcl_ap_node listen-frame <capture.wav> [block]
```

## Maturity

| | Status |
|---|---|
| [`spec/ap-bootstrap-1.md`](spec/ap-bootstrap-1.md) — `AP-BOOTSTRAP-1` | **Candidate.** Normatively complete and implementable. |
| Acoustic transport identifier `1` | Assigned for use, not frozen |
| [`spec/ap-v0.md`](spec/ap-v0.md), [`spec/bootstrap-link-convergence.md`](spec/bootstrap-link-convergence.md) | Research Draft — wider profile families and adaptive link convergence |

Per-document maturity for all of MCL is in
[`mcl-core/SPECIFICATION_INDEX.md`](https://github.com/machine-contact-layer/mcl-core/blob/main/SPECIFICATION_INDEX.md).

## Documents and verification

- [`spec/ap-bootstrap-1.md`](spec/ap-bootstrap-1.md) — the bootstrap profile
- [`conformance/vectors/VECTORS.md`](conformance/vectors/VECTORS.md) — the audio conformance vectors
- [`experiments/README.md`](experiments/README.md) — the experimental program, with raw captures and results
- [`experiments/008-embedded-node/`](experiments/008-embedded-node/) — the full stack decoding over the air on an ESP32-S3

## Related repositories

[mcl-core](https://github.com/machine-contact-layer/mcl-core) ·
[mcl-link](https://github.com/machine-contact-layer/mcl-link) ·
[mcl-sdk](https://github.com/machine-contact-layer/mcl-sdk) ·
[mcl-ble](https://github.com/machine-contact-layer/mcl-ble) (where a contact
usually continues) · [mcl-ip](https://github.com/machine-contact-layer/mcl-ip)

## License

Apache-2.0. See [`LICENSE`](LICENSE).
