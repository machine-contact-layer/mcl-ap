# MCL-AP Profile Evolution Rules

Status: **Research Governance Draft**

MCL-AP is an important reference binding, but MCL must remain valid if acoustic hardware changes radically over the coming decades.

## 1. AP is a binding, not the constitution

No MCL Core semantic requires acoustics.

A machine may implement MCL over another standardized binding while preserving the same Core/Wire/Link contracts.

## 2. Profile IDs are immutable after stabilization

Once an AP profile ID is Stable:
- its required waveform/framing behavior is fixed;
- its meaning is not changed to track newer algorithms;
- incompatible PHY changes receive a new profile ID.

Adaptive parameters explicitly permitted by the profile may vary without changing the ID.

## 3. Research families are not assigned profiles

`AP-B0`, `AP-R1`, `AP-W2`, and `AP-X` are currently research/profile-family names.

They MUST NOT receive permanent numeric profile IDs until their mandatory behavior and conformance tests are defined.

## 4. Bootstrap longevity

A future Stable acoustic bootstrap should optimize for:
- discoverability across heterogeneous paths;
- bounded acquisition behavior;
- deterministic protocol identification;
- explicit version/profile negotiation;
- false-alarm control;
- graceful coexistence with newer profiles.

It should not encode assumptions that all future machines use one sample rate, one frequency band, one modulation family, or matched transducers.

## 5. Physical evidence is advisory

AP may expose measured:
- time of flight;
- direction;
- channel state;
- frequency response;
- timing/time-scale estimates.

These are physical observations.
They MUST NOT by themselves create identity, authority, or proof of co-presence.

## 6. Device capability versus path capability

Advertised hardware capability is informative.
Profile selection SHOULD be based primarily on the measured directional end-to-end path when measurement is available.

A->B and B->A may use different profiles under one logical MCL session.

## 7. Qualification boundary

A future AP conformance program should distinguish:
- exact waveform/profile conformance;
- detector/decoder performance under defined reference channels;
- cross-device interoperability;
- ecosystem robustness under real hardware diversity.

A single reference laptop/phone pair is never sufficient evidence for a Stable general profile.
