# AP-BOOTSTRAP-1 Requirements

Status: **Research Draft**

This is not the profile. It states what `AP-BOOTSTRAP-1` must do, what it must
refuse to do, and which parameters are **open** — to be closed by measurement
rather than by preference. The normative profile is written after the bake-off,
from these requirements plus its results.

## 1. What this profile is for

`mcl-core/spec/conformance-profiles-v1.md` §5 makes `AP-BOOTSTRAP-1` the single
mandatory-to-implement rendezvous path of `MCL Stranger-Contact 1`. It is the
one thing two machines that share no network, no credential and no prior
arrangement are guaranteed to have in common.

That is its whole job. It is a doorway, not a channel.

Everything else — sessions, sequence numbers, migration control, authentication,
bulk carriage — happens after migration to a richer bearer, or in a later
`AP-LINK-1` profile. Neither belongs here, and §3 says why in terms strong
enough to survive the pressure to add them.

## 2. Carriage: exactly the first-contact kernel

**Carries.** Raw canonical Wire major-1 Tier-0 objects, and only these kinds:

```text
PRESENCE            10 bytes
TRANSPORT_OFFER     17 bytes   <- the worst case, and the one not yet measured
TRANSPORT_ACCEPT    16 bytes
```

These are the Stable kernel of `mcl-core/governance/V1_SCOPE.md` §3.2. The
Tier-0 ceiling is 17 bytes, so the largest legal bootstrap payload is bounded
and known before any waveform is chosen.

**Does not carry.** Link frames — no frame class, no session reference, no
sequence, no destination. The bootstrap path is a broadcast doorway on an
observable medium and has nothing to put a session on.

**MUST NOT carry**, and a conformant receiver MUST refuse rather than decode:

- credentials, certificates, public keys, key shares, signatures or MACs;
- any object kind outside the three above;
- arbitrary or caller-supplied payloads;
- anything whose length exceeds the Tier-0 ceiling.

### 2.1 Why the prohibition is structural, not advisory

A profile that says "please do not send credentials here" will carry
credentials. This one removes the facility instead:

1. The payload is a Wire major-1 Tier-0 object or it is refused. There is no
   opaque-bytes mode and no escape hatch to add one.
2. The admissible kind set is three values, checked at decode.
3. The maximum payload is the Tier-0 ceiling, which no useful credential fits
   inside. `research/TWO_BUILDER_AUDIT.md` §3.3: a bare Ed25519 signature alone
   is 64 bytes.

### 2.2 What that property is, stated precisely

The claim is **not** that a bootstrap frame cannot leak a credential. It cannot
be, and a profile must not assert an information-flow guarantee its wire format
has no way to enforce — `ARCHITECTURE_CHARTER.md` §2.11 forbids naming a
mechanism for a property it lacks.

A sender can always encode secret material covertly into ordinary fields. A
receiver cannot determine whether a `source_ref` or an `endpoint_token` was
derived from a private key, and no decoder can.

The defensible property is:

> `AP-BOOTSTRAP-1` has **no credential-bearing and no opaque-payload facility**.
> Conforming senders **MUST NOT** encode sensitive identity, credential, key or
> authentication material into bootstrap fields.

That is a normative prohibition on senders plus the removal of the mechanism a
*conforming* use would otherwise reach for. It is strong and it is enforceable
at the decoder for everything except deliberate covert encoding, which is
outside what any wire format decides.

The result is that first contact stays public and minimal, which is what lets
`MCL Stranger-Contact 1` promise contact between strangers without promising
them any privacy on the acoustic medium — a promise the medium could not keep.
Acoustic reception is proximity evidence, never proof of co-presence, and the
medium is observable, injectable and relayable.

## 3. Shared air is the hard requirement

A street is not a laboratory. Ten machines that hear one `PRESENCE` and answer
at once defeat a perfect modem, and no amount of coding gain recovers a
collision. This section is a first-class requirement, not a refinement.

The profile MUST specify:

- **Listen-before-transmit.** An energy or acquisition test, its window, and its
  threshold. A machine that transmits into an occupied medium is not conformant.
- **Reply scheduling.** A responder answers in a bounded window, positioned so
  that independent responders do not collide by construction. Randomised
  offsets, slotting, or both — selected by measurement.
- **Duplicate suppression.** A repeated announcement from one source within a
  bounded interval is recognised as the same announcement, not a second machine.
- **Backoff and retry bounds.** Both bounded, and both stated as numbers a
  constrained implementation can honour.
- **Multi-responder behaviour.** What an initiator does when several machines
  answer. Contact with several peers is the normal case on a street, and the
  SDK today holds one contact per node (`V1_SCOPE.md` §5.5) — the profile must
  not assume otherwise.

**Test obligation.** Three or more machines, measured, not argued.

## 4. Receiver bounds a constrained machine can honour

The target is the class of machine already demonstrated in Experiment 008: an
ESP32-S3 with 512 KB SRAM and no double-precision FPU. The profile MUST state,
as numbers:

- peak working set for receive, caller-owned, no heap;
- worst-case acquisition work per unit of audio;
- whether a decode can be performed within a bounded latency while other work
  runs;
- the arithmetic precision required — single precision, or fixed point.

The current modem is a useful reference point rather than a target: its receive
scratch is about 77 KB and its acquisition searches at a stride before refining.
An implementation that needs the whole capture in memory has failed this
requirement, not merely performed badly.

## 5. Open parameters — closed by the bake-off, not here

Nothing in this list has a value yet. Recording them as open is the point:
`mcl-ap/registries/ap-profiles-v0.1.json` states that 3/6 kHz must not be
standardised merely because it rescued one hardware campaign, and
`research/TWO_BUILDER_AUDIT.md` §3.2 records that Experiment 008 measured 9/10
recovery at 10 bytes falling to 2/10 at 24.

| Open parameter | Closed by |
|---|---|
| Carrier frequencies and spacing | measured path response across the retained corpus, not one room |
| Modulation and symbol rate | recovery at the bootstrap payload sizes, across devices |
| Preamble form and duration | detection probability at a calibrated false-alarm rate |
| Acquisition threshold and search bound | Pd/Pfa on real captures, including the lead-in variation that broke a fixed bound before |
| Coding: interleaving | **evidence argues strongly against it.** Experiment 010 found errors isolated, not bursty — the burst assumption behind interleaving is not supported by any retained capture |
| Coding: FEC | **deferred, and 010b argues against reaching for it first.** A block code sized for a ~30% frame loss driven by drift would pay airtime to carry a problem better acquisition removes. Revisit after the timing remedy is measured over air |
| Timing recovery | **confirmed important, and now the indicated remedy.** Experiment 010b: a frame fails when accumulated drift reaches ~0.38 of a symbol, so usable length is about 60 / \|sps error\| bits. The estimator searches in 0.05 steps across ±0.5 of the true rate and still returned values 0.4–0.45 away — **the right answer was inside the grid and was not chosen**, making this a scoring problem, not a resolution one. A longer timing reference or a mid-frame pilot, not a finer search. **Experiment 010c tested this and it holds**: a blind whole-frame estimator, implementable by a receiver, took CRC recovery 3/10 to 5/10 on the hardest cell and 9/10 to 10/10 on two others, across two rigs. The residual 0.045-0.058 deviation is partly genuine crystal offset between devices, which estimation cannot remove, so the timing budget has two terms |
| Decision threshold placement | the measured error asymmetry |
| Frame length ceiling | structurally fixed at <= 17 B by §2. Experiment 010b now bounds the worst case from the timing term alone: `TRANSPORT_OFFER` at 17 B is predicted clean in only **14 of 20**, against 20/20 for a 10-byte `PRESENCE`. **Marginal, not comfortable**, and the physical campaign should be expected to do worse. Still the next campaign |
| Silence, guard and turnaround intervals | the contention requirements of §3 |

## 6. The bake-off must measure structure before choosing coding

This is the requirement most easily got wrong, and the reason this document
exists before the profile.

Coding choices are not interchangeable, and the right one is determined by how
the errors actually arrive:

```text
errors independent and symmetric   ->  block FEC pays
errors bursty                      ->  interleaving first; naive repetition wastes airtime
errors asymmetric                  ->  decision threshold placement, before any coding
errors growing with bit index      ->  timing recovery, and no coding fixes it
```

Two of these are already indicated and neither is settled: a prior analysis
found 97% of bit errors running one direction, and recovery falls sharply with
frame length, which is the signature of accumulating timing error rather than
of noise.

**Adding FEC to a timing problem buys nothing and costs airtime.** The bake-off
therefore measures the error structure of the retained corpus *first*, with
exact ground truth, and only then evaluates coding against it.

The retained corpus is 123 captures across five experiments, three device
families and both directions. Ground truth is recorded per campaign — Experiment
003, for example, records the expected payload `00 02 00 00 00 01 01 00 00 01 3C`.

**Measured, 2026-09-05, in `experiments/010-bootstrap-bakeoff/`.** Errors are
isolated rather than bursty (58 of 67 runs are single bits at 24 bytes, and every
error is isolated in all three short cells), asymmetric (77–100% run `1→0`),
low-margin, and concentrated in the frame tail — every error in all three short
cells falls in the second half. Length hurts superlinearly: 10 bytes gives
0.192% BER and 9/10 recovery, 24 bytes gives 3.889% and 3/10. A timing sweep
removes 100% of the errors at 10 bytes and 53.6% at 24, so the long-frame regime
is roughly half accumulated timing error and half genuine marginal SNR.

The consequence for this profile is §2, not a coding choice: **the strongest
lever is frame length, and the Tier-0 ceiling already bounds it.** The 17-byte
cap was adopted in §2.1 for a structural reason, and it independently places the
profile **below the measured 24-byte failure regime**. The 24-byte regime that
motivated the search for FEC is one `AP-BOOTSTRAP-1` never enters.

**It does not place the profile in a regime measured to work, and this document
must not be read as saying so.** The bootstrap exchange is not one 10-byte
object. It is three:

```text
PRESENCE           10 bytes    measured, 0.192% BER, 9/10
TRANSPORT_OFFER    17 bytes    NOT MEASURED
TRANSPORT_ACCEPT   16 bytes    NOT MEASURED
```

The degradation between 10 and 24 bytes is superlinear — 2.4× the payload for
20× the error rate — which makes interpolation to 16 and 17 bytes precisely the
inference the data forbids. **The worst-case bootstrap objects are the ones that
have not been measured.** Until they are, the coding decision stays open.

The eventual criterion is also not raw BER. It is end-to-end contact success
within a bounded retry window: 90% one-shot recovery is adequate with a cheap
retry and useless if contention makes every retry expensive, so §3 and this
section close together or not at all.

## 7. Synthetic impairment is a supplement, never the evidence

Synthetic noise, drift and offset let a candidate be swept where the corpus is
thin. They do not establish that a profile works, because the failures that
mattered in this project were not the ones that get simulated: a notch in one
laptop speaker took a candidate from 9/10 to 0/10, and a fixed acquisition
bound decoded 9/10 of board captures and 0/10 of laptop captures of the same
transmissions.

Selection evidence is over-air. Synthetic results are reported separately and
never substituted.

## 8. What closes this document

`AP-BOOTSTRAP-1` is specified when every row of §5 has a value and a measurement
behind it, §3 has a multi-machine campaign, and §4 has numbers from a
constrained target. Then:

1. the normative profile is written from the prose, independently;
2. a clean-room encoder and decoder are built from that prose alone;
3. PCM vectors and negative tests are published with it;
4. a physical cross-implementation campaign runs;
5. a **new** Standards Action identifier is assigned.

Profile 192 stays Experimental Use permanently and is not mutated into this.
`GOVERNANCE.md` §4.3: an Experimental Use value is never relabelled Stable.

**Expect step 2 to send work back to the bake-off.** A clean-room reader finds
the ambiguities a bake-off cannot, exactly as `conformance/independent/SPEC_GAPS.md`
did for Wire. That is the process working, not a setback, and the schedule
should carry it.
