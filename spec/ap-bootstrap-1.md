# AP-BOOTSTRAP-1

**Status:** **Candidate.** Normatively complete and implementable from this
document alone. Not Stable: see §11, which states exactly what is missing and
why the missing thing is a second transmitter rather than more text.

**Layer:** MCL-AP, transport 1
**Profile identifier:** not yet assigned. See §10.
**Supersedes:** nothing. Experimental profile 192 is a different profile, is not
mutated by this document, and remains Experimental Use forever.

---

## 1. What this profile is for

`AP-BOOTSTRAP-1` carries a raw MCL Wire Tier-0 object between two machines that
have **no prior arrangement of any kind** — no address, no port, no MAC, no
`endpoint_token`, no shared secret, no pairing step, no out-of-band channel.

That is the only thing it is for, and the scope is deliberately narrow. Every
other MCL bearer presupposes that an offer already reached the peer somehow: IP
defers discovery and assigns no port, and a BLE peer is found by an
advertisement a conformant implementation may omit. Acoustic is the one bearer
that needs nothing arranged in advance, which is why the guaranteed
interoperability floor rests on it.

**It is not a general acoustic link.** It carries one small object per
transmission, has no fragmentation, no reliability, no ordering and no flow
control. `AP-LINK-1`, if it is ever written, is a different profile.

## 2. What it is not, stated before the parameters

**No confidentiality, no integrity, no authentication, no replay protection.**
The CRC-16 in §5 detects accidental corruption and nothing else; anyone in
acoustic range can read every byte, and anyone can emit bytes that this profile
will accept.

**It has no credential-bearing and no opaque-payload facility.** There is no
field in which a key, a signature, a token or an authentication exchange can be
placed. Conforming senders **MUST NOT** encode sensitive identity, credential,
key or authentication material into any field of this profile, directly or by
encoding it into a value whose stated purpose is something else
(`ARCHITECTURE_CHARTER.md` §2.11).

This is a property of the format plus a requirement on senders. **It is not an
information-flow guarantee**, and the format cannot enforce one: a sender that
puts a secret into `source_ref` produces bytes this profile will happily
transmit. The absence of a facility removes the obvious way to do it; it does
not make it impossible.

**Authentication belongs after migration.** The intended shape is: meet here,
credential-free and in public; migrate to a richer bearer; authenticate there.
Nothing in MCL forces a signature through air, and `MCL_AP_MODEM_MAX_PAYLOAD_BYTES`
is 64, so a bare Ed25519 signature would not fit if one tried.

## 3. Physical layer

All values are normative. A receiver implementing this profile MUST use them;
they are not defaults.

| Parameter | Value |
|---|---|
| Sample rate | 48000 Hz |
| Symbol rate | 300 baud, exactly 160 samples per symbol |
| Modulation | Binary FSK, continuous phase across every symbol boundary |
| Tone for bit 0 | 3000 Hz |
| Tone for bit 1 | 6000 Hz |
| Bit order | MSB first within each byte |
| Sample format | Signed 16-bit PCM, monaural |

**Phase continuity is normative, not stylistic.** A phase discontinuity at each
bit edge spreads energy across the band, and the receiver's detection windows in
§6 sit exactly on those edges.

### 3.1 Frame envelope

Emitted in this order, with no gaps:

| Section | Duration | Content |
|---|---|---|
| Leading silence | 0.1 s | zero samples |
| Preamble | 0.2 s | linear-FM chirp, 2000 Hz → 6000 Hz |
| Training | 16 symbols | the byte `0x55` twice, alternating 0 and 1 |
| PHY header | 3 bytes | §5 |
| Payload | 1–64 bytes | §4 |
| Trailing silence | 0.5 s | zero samples |

The chirp sweeps linearly in frequency. Its phase MUST be accumulated in a
representation of at least double precision: the quadratic term reaches
2π·6000·0.2 radians, and a single-precision mantissa loses the fractional part
of an angle that size before the chirp ends, producing a reference that stops
matching the transmitted waveform at its own tail — exactly where a correlation
peak is decided.

The chirp's amplitude is scaled so its mean energy per sample is 0.40 relative
to full scale. Receiver correlation is normalised and therefore scale-invariant,
so this fixes the preamble's level **relative to the FSK section** and nothing
else.

**Trailing silence MAY be reduced to 0.1 s** by a transmitter whose buffer
cannot hold more. It is padding, not signal: the receiver locates the payload
from the preamble and needs *its own* capture to extend past the last symbol,
not the transmitter's silence. A receiver MUST NOT depend on trailing silence
being present.

**Emitted amplitude is not specified and is not part of conformance.** Two
implementations at different volumes are both conforming.

## 4. Payload

The payload is **one raw MCL Wire Tier-0 object**, encoded at
`MCL_WIRE_STABLE_MAJOR`, with no Link frame around it.

Carrying a bare Tier-0 object rather than a Link frame is deliberate: the Link
frame adds 8 bytes of header plus optional fields, and §9 shows that length is
the dominant term in acoustic recovery. A 24-byte Link frame carrying a 10-byte
`PRESENCE` was measured at 3/10 where the bare object reached 9/10.

Payload length is 1 to 64 bytes. The three objects this profile exists to carry
are:

| Object | Bytes at major 1 |
|---|---:|
| `PRESENCE` | 10 |
| `TRANSPORT_ACCEPT` | 16 |
| `TRANSPORT_OFFER` | 17 |

A receiver MUST NOT assume the payload is one of these three. It MUST decode it
as a Tier-0 object and refuse anything that does not decode.

## 5. PHY header

Three bytes, immediately after the training symbols:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | payload length in bytes, 1–64 |
| 1 | 2 | CRC-16/CCITT-FALSE over the payload, big-endian |

CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value `0xFFFF`, no reflection
of input or output, no final XOR.

The CRC covers **the payload only**, not the length byte. A receiver MUST reject
a frame whose declared length is 0 or greater than 64 before computing anything
over it.

## 6. Receiver

### 6.1 Acquisition

Locate the preamble by normalised cross-correlation against a locally generated
reference chirp. A frame is acquired when the normalised correlation magnitude
is **at least 0.40**.

Normalisation is required, not optional: an un-normalised correlation peaks on
loud noise.

### 6.2 Initial timing estimate

From the 16 training symbols, estimate the symbol phase, the symbol rate, and
the decision bias — the offset that separates the two tones' log energy ratio.

**The decision bias is not zero and MUST be estimated.** An acoustic path does
not present two frequencies equally; Experiment 002 measured one tone 19–21 dB
down inside a notch. 97% of bit errors in the retained corpus ran one direction,
which is what an unestimated bias looks like.

### 6.3 Symbol decision

For each symbol, compute the energy at 3000 Hz and at 6000 Hz over that symbol's
160-sample window — a Goertzel evaluation is sufficient and is what the
reference implementation uses. The bit is 1 when `log(E₁/E₀) − bias > 0`.

### 6.4 Whole-frame rate refinement — REQUIRED

**If the first decode attempt fails, a conforming receiver MUST retry with a
refined symbol rate before reporting failure.**

This is normative because it is the difference between a working rendezvous and
a broken one, and §9 measures it.

The refinement:

1. For each candidate rate from `nominal − 0.6` to `nominal + 0.6` samples per
   symbol in steps of `0.01`, where `nominal` is 160.0:
2. Recompute the payload start as `phase + 16 × candidate`. The phase MUST be
   recomputed per candidate: the training symbols precede the payload, so
   changing the rate moves where the payload begins, and holding the old start
   would measure a different frame rather than a better rate.
3. Score the candidate by the **mean decision margin** `mean |log(E₁/E₀) − bias|`
   over the frame.
4. Decode at the highest-scoring rate.

The score uses **only the signal**. It never uses the expected payload, so it is
something a receiver can actually run.

**Why it works.** The initial estimate fits the rate to 16 symbols — 2560
samples. That is too short a baseline: Experiment 010b showed the search already
covered the true rate and still returned a 0.4–0.45 sample error, so the right
answer was inside the grid and was not being chosen. It was a scoring problem,
not a resolution problem, and the fix is more evidence rather than a finer grid:
~160 symbols instead of 16.

**Two terms, only one of which is removable.** After refinement the estimated
rate settles 0.045–0.058 away from nominal rather than at zero, because the
transmitter's and receiver's oscillators are independent and the true received
rate is 160.0 × a clock ratio. Refinement removes estimation error; it cannot
remove genuine clock offset. A frame stops decoding at roughly **0.38 of a
symbol** of accumulated drift, giving a usable length of about `60 / |rate error|`
bits — which is why this profile's objects are small and why it has no
fragmentation.

### 6.5 No forward error correction

This profile specifies **no FEC and no interleaving**, and that is a measured
decision rather than an omission.

Experiment 010 measured the error structure before any coding scheme was
considered: errors are **isolated** (58 of 67 error runs are single bits),
**asymmetric** (77–100% one direction), **low-margin**, and **concentrated in
the frame tail**. That is a timing signature, not independent bit noise.
Interleaving addresses bursts that do not occur here, and a block code sized for
the observed loss would spend airtime — the scarcest resource on this bearer —
carrying a problem that §6.4 removes.

## 7. Transmission behaviour, retries and contention

**Contention is protocol, not modulation.** Ten machines that answer one
`PRESENCE` simultaneously defeat a perfect modem: the replies overlap, all are
lost, and no amount of error correction helps.

| Parameter | Value | Meaning |
|---|---|---|
| Announce interval | 1500 ms | between successive `PRESENCE` emissions |
| Maximum announcements | 10 | before giving up and reporting nothing heard |
| Reply slot | 400 ms | window a reply is randomised within |
| Response timeout | 3000 ms | before an unanswered offer is retried |
| Offer retries | 2 | per bearer, before moving to the next |

A receiver that hears a `PRESENCE` and intends to reply **MUST NOT reply
immediately.** It MUST delay by a value drawn uniformly from the reply slot.

The delay SHOULD be drawn from a random source. Where none is available it MAY
be derived from the replying machine's `source_ref`. **A deployment relying on
the derived form must understand what it gets:** it separates two peers
reliably and three only by coincidence, because the delay is then a pure
function of `source_ref` and two machines whose references are congruent modulo
the slot width collide every single time.

**Retry the same bearer before concluding anything about the peer.** On this
bearer a lost offer and an unsupported bearer are indistinguishable, and §9
measures the 17-byte `TRANSPORT_OFFER` failing often enough that a single
unanswered offer is no evidence at all about what the peer supports.

## 8. Finding a common bearer

A peer that hears a `PRESENCE` learns that a machine is present and **nothing
about what it speaks.**

This is a property of major-1 `PRESENCE`, not a limitation of this profile:
`machine_class` was removed from the major-1 body, and `capability_tag` is
normatively a sender-controlled opaque revision token that a receiver is
**forbidden** to compare across peers.

A common bearer is therefore found by **ordered trial**, not by intersection.
Each peer offers the bearers its deployment mandates, in the order the
deployment profile fixes, and `TRANSPORT_ACCEPT` or a timeout answers. Two
independent builders converge because they were handed the same ordered list,
not because they exchanged one.

Simultaneous offers **MUST** be resolved by the existing rule in
`mcl-link/spec/link-contact-ownership-v0.1.md`: the larger
`(source_ref, migration_ref)` key wins the controller role, and an exact tie
aborts both transactions. Peers that simply accept each other's offers end up
holding two live transactions on one contact.

## 9. Evidence

Experiment 011, 2026-09-06. DFR1154 speaker → laptop microphone, independent
clocks, 15 trials per cell, one session and one level.

| Object | Bytes | Acquired | Without §6.4 | With §6.4 |
|---|---:|---:|---:|---:|
| `PRESENCE` | 10 | 15/15 | 9/15 | **15/15** |
| `TRANSPORT_ACCEPT` | 16 | 15/15 | 8/15 | **15/15** |
| `TRANSPORT_OFFER` | 17 | 15/15 | 4/15 | **14/15** |

21/45 → 44/45. Both columns are computed from the **same captures**, so the
difference is the receiver and not a better room.

Offline regression on the previously retained corpus, no new transmission:
10-byte cell 9/10 → 10/10, 24-byte cell 3/10 → 5/10.

## 10. Registry

**No profile identifier is assigned by this document.**

Assignment requires the `mcl-core/governance/REGISTRY_POLICY.md` §2
requirements and, for a Standards Action value, the interoperability evidence
that §11 says does not yet exist. Assigning a number now and justifying it later
is the failure the registry policy was written to prevent.

Experimental Use profile 192 is **not** this profile and is not relabelled.

## 11. Why this is Candidate and not Stable

One thing is missing, and it is not more text.

**Every measurement of this waveform comes from one transmitter class.** The
3000/6000 Hz pair was chosen on the DFR1154's speaker and confirmed on the same
speaker. The `mcl-ap` band registry already records that this pair must not be
standardised on the strength of one campaign, and Experiment 011 is a second
campaign on the same transmitter rather than a second transmitter.

There is direct evidence that the choice does not travel: **this laptop's
speaker has a measured notch around 3 kHz**, one of the two tones, and a
loopback path through it recovered 1 of 3 at 10 bytes where the board rig
recovers 9 of 15. A bootstrap profile whose lower tone lands in a common laptop
speaker's notch is not yet a bootstrap profile.

Promotion to Stable requires:

1. **Outstanding.** The band swept on **at least two transmitter classes that
   are not the DFR1154**, with the pair selected from that evidence rather than
   inherited. This is the load-bearing gap.
2. **Partly done.** A receiver written from this document alone now exists —
   `conformance/independent/ap_bootstrap_rx.py`, sharing no code with `mcl-ap`,
   in a language that cannot accidentally link it — and it agrees with the
   reference on all nine vectors in both directions
   (`conformance/check-vectors.sh`).

   That demonstrates this document is **self-sufficient**: no parameter a
   receiver needs lives only in the reference implementation. It does **not**
   demonstrate the document is unambiguous to someone who has never seen that
   implementation, because the same author wrote both. The remaining half needs
   a different reader.
3. **Done.** `conformance/vectors/`, three positive and six negative, each
   negative refusing for a different reason. Their limit is stated in the
   manifest and repeated here because it matters: they are noise-free, so a
   receiver that omitted §6.4 entirely would pass them. Timing recovery is what
   fails in a room, and these vectors have perfect timing.
4. **Outstanding.** A **three-or-more machine contention campaign**, since §7's
   parameters are currently reasoned rather than measured.
5. **Outstanding.** A profile identifier assigned under §10.

Two of the five are closed. The two that remain outstanding both need something
this tree cannot contain — a second class of transmitter, and a third machine —
and neither is closed by more text.

Until then a deployment may implement `AP-BOOTSTRAP-1` and MUST NOT describe it
as frozen.
