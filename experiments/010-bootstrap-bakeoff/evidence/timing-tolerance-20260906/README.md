# Evidence: timing tolerance versus frame length, 2026-09-06

Derived, not captured. **No transmission, no hardware, no new claim about a
link.** Produced by `../../timing_tolerance.c`.

## What was measured

One deterministic property: the largest symbol-rate error a frame of a given
length survives, using the real major-1 objects encoded by `mcl-wire`.

No noise model and no SNR are invented — `spec/ap-bootstrap-requirements-v0.1.md`
§7 forbids treating synthetic impairment as selection evidence. Only the symbol
rate is perturbed, because Experiment 010 identified residual symbol-rate error
accumulating with bit index as the dominant recoverable mechanism, and because
rate is the term that accumulates while phase is not.

The "predicted clean / 20" column applies each tolerance to the symbol-rate
errors the shipped receiver **actually produced** on the twenty retained
board-to-host captures in `../error-structure-20260905/`. Those are estimator
errors against a true 160.0 samples per bit, not assumptions.

## Result

```text
object              bytes   bits  |sps| tol  drift@fail  of symbol  clean/20
PRESENCE               10    104      0.570        59.3      0.370     20/20
TRANSPORT_ACCEPT       16    152      0.410        62.3      0.389     16/20
TRANSPORT_OFFER        17    160      0.390        62.4      0.390     14/20
```

Drift at failure is near-constant at 0.37–0.39 of a symbol, which states the
mechanism as a law: **usable length is roughly 60 / |sps error| bits.**

## What it is not

A prediction, and an optimistic one. It holds the timing term alone and
everything else perfect, while Experiment 010 measured that roughly half the
real errors at 24 bytes were **not** timing-removable. The physical campaign
should be expected to do worse than this line.

`AP-BOOTSTRAP-1` remains unspecified and AP-B0 remains NOT SELECTED.
