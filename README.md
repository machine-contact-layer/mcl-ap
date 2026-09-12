<p align="center">
  <img src=".github/banner.png" alt="OJOBIT" width="100%">
</p>

<h1 align="center">MCL-AP</h1>

<p align="center"><strong>First contact through the air, using the speaker and microphone the machine already has.</strong></p>

<p align="center">
  <a href="https://github.com/machine-contact-layer/mcl-ap/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/machine-contact-layer/mcl-ap/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/machine-contact-layer/mcl-ap/blob/main/LICENSE"><img alt="License Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue"></a>
  <img alt="profile" src="https://img.shields.io/badge/AP--BOOTSTRAP--1-Candidate-yellow">
  <img alt="evidence" src="https://img.shields.io/badge/over--air-E4-brightgreen">
</p>

<p align="center">
  <a href="https://github.com/machine-contact-layer/mcl-sdk"><b>Use the SDK instead</b></a> ·
  <a href="https://github.com/machine-contact-layer/mcl-core"><b>Specifications</b></a> ·
  <a href="https://github.com/machine-contact-layer/mcl-ap/tree/main/experiments"><b>Experiments</b></a>
</p>

---

> ### Most people should start with the SDK, not here
>
> This repository is a **specification**. If you are building a product, you
> want [**mcl-sdk**](https://github.com/machine-contact-layer/mcl-sdk) — one CMake project, no sibling checkout, and a
> working example you can run in about a minute. Come back here when you need
> to know exactly what a byte means, or when you are writing an independent
> implementation.

## Why this exists

Two machines with no shared network still share the air. MCL-AP is the binding
that needs no infrastructure at all: no access point, no pairing, no account —
just a speaker on one side and a microphone on the other, long enough to agree
on somewhere better to talk.

It is the bootstrap medium for `MCL Stranger-Contact 1`, and it has been run
over real air on real hardware, including the whole stack decoding on an
embedded target with no host in the loop.

> **`AP-BOOTSTRAP-1` is a Candidate profile, not Stable.** Promotion needs a
> second independent transmitter class. Profile identifier 192 stays
> Experimental Use.

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

## Being called without waiting for a call

Every acoustic result in this repository was, until now, a *scheduled* decode:
a recording was started, a frame was sent into it, and the whole recording was
handed to the decoder afterwards. Both peers knew when the exchange would
happen. A machine doing its actual job does not — it is welding, or moving a
pallet, or sitting in a dock, and a stranger walks up and transmits. If a
receiver only listens during windows it chose, that call is not heard, and
"not heard" looks exactly like "nobody was there".

[`include/mcl/ap_listen.h`](include/mcl/ap_listen.h) is the receiver driven the
other way: audio is pushed in whatever blocks the capture path produces, and
the machine polls whenever it gets a moment.

- **Acquisition is incremental, not retrospective.** Every sample is
  correlated against the preamble once, when it first becomes searchable —
  never once per poll. A listener therefore costs the same whether its window
  holds two seconds or sixty, and whether it is polled once a second or a
  hundred times. `samples_searched` reports that directly and the test asserts
  it.
- **The window is scheduling slack, not memory.** It is how late the machine
  is allowed to be, so size it by the longest stretch the application can go
  without polling.
- **Audio dropped unheard is counted.** `samples_unscanned` is the one failure
  a listener could hide perfectly, because a missed call and a quiet room are
  the same silence. A machine too busy to listen says so.

There is no anomaly detector and there should not be. The preamble correlator
*is* the detector — a matched filter for exactly the thing being looked for,
already 10/10 on real over-air captures, producing a normalized score against
a threshold rather than a probability that needs interpreting.

Streaming the archived over-air captures past the listener in small blocks,
with no knowledge of where the frame sits, reproduces the one-shot decoder
**exactly** — at every block size from 256 to 8192 samples:

| capture set | decoded whole | streamed, 256 / 1024 / 4096 / 8192 |
|---|--:|--:|
| 009 handset, 10-byte object | 5/10 | 5/10 5/10 5/10 5/10 |
| 009 handset, 24-byte frame | 2/10 | 2/10 2/10 2/10 2/10 |
| 008 board, 10-byte object | 9/10 | 9/10 9/10 9/10 9/10 |
| 008 board, 24-byte frame | 3/10 | 3/10 3/10 3/10 3/10 |

Continuous listening costs nothing in recovery rate. Run it yourself:

```text
mcl_ap_node listen-wire  <capture.wav> [block]
mcl_ap_node listen-frame <capture.wav> [block]
```

Hearing a call is still only reception. The listener does not decide who sent
the frame, whether the payload is true, or that the machine owes anyone a
reply — `reception != identity != authenticity != authority != trust !=
obligation` is enforced above this layer and nothing here weakens it.

## Documents

- [`spec/ap-v0.md`](spec/ap-v0.md) — profile architecture
- [`spec/bootstrap-link-convergence.md`](spec/bootstrap-link-convergence.md) — bootstrap and convergence procedure
- [`experiments/README.md`](experiments/README.md) — experimental program

## Status

Public research binding. Not an adopted standard. AP-B0 remains unselected for
the Stable v1.0 surface; Candidate bootstrap work, retained captures and
negative experiments remain part of the research record.

**MCL-AP is Experimental and AP-B0 is not selected.** The waveform has now been
carried by three different loudspeakers — a laptop, an ESP32-S3 and an Android
handset — and recovers between 1/10 and 9/10 depending on payload length and
device. Acquisition is reliable; recovery is not. That is an honest description
of a research binding and not of a link anything should depend on.
