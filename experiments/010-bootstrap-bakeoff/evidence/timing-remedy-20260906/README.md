# Evidence: whole-frame blind rate refinement, 2026-09-06

Offline re-analysis of captures already retained. **No new transmission and no
hardware.** Produced by `../../timing_remedy.c`.

## What was tested

Experiment 010b diagnosed the symbol-rate estimator as a **scoring** problem
rather than a resolution one: the search covers ±0.5 around the nominal 160.0 in
0.05 steps, the right answer is inside the grid, and it is not chosen. Sixteen
training bits — 2560 samples — is too short a baseline.

That was a hypothesis. This tests it on real captures before a hardware campaign
is spent on it.

## The candidate estimator is blind

It uses **no ground truth**, so it is something a receiver can actually run. The
metric is the mean decision margin over the whole frame:

```text
score(sps) = mean over bits of | log_ratio(bit, sps) - bias |
```

At the correct rate every symbol is sampled near its centre and the two tone
energies separate cleanly, so the mean margin is maximal. At a wrong rate the
later symbols straddle boundaries and it falls. The estimate uses ~160 bits of
evidence instead of 16, which is the change 010b argued for.

**This is not the `--sweep` diagnostic** in `error_structure.c`. That one scores
against the known payload and is an upper bound no receiver can reach. This
scores against nothing but the signal. The payload is supplied here only to
check the CRC afterwards.

## Result

| Cell | payload | CRC before | CRC after | mean \|sps err\| before | after |
|---|--:|--:|--:|--:|--:|
| 008 board→host frame | 24 B | 3/10 | **5/10** | 0.155 | 0.045 |
| 008 board→host wire | 10 B | 9/10 | **10/10** | 0.145 | 0.058 |
| 003 board→laptop | 11 B | 9/10 | **10/10** | 0.250 | 0.045 |

Every cell improves, on two different rigs. Across all three, 21/30 to 25/30.

## The residual is probably not error

After refinement the mean deviation from the nominal 160.0 settles at 0.045–0.058
in all three cells and does not go to zero.

That is expected and is not the estimator failing. Transmitter and receiver have
independent crystals, so the **actual** received symbol rate is 160.0 times a
clock ratio and is not exactly 160.0. Trial 03 of the 24-byte cell is the visible
case: its deviation from nominal grew 0.000 → 0.070 while its CRC went from
failing to passing. Measured against nominal that looks worse; measured against
what the capture actually contains, it is right.

**So "estimator error" against 160.0 conflates two things** — estimator error and
genuine clock offset — and only the first is fixable by estimation. The tolerance
law in 010b is unaffected, because it bounds accumulated drift regardless of
cause.

## What this does not establish

Nothing over air. This shows a better estimator recovers more of the **retained**
captures; it does not show what a rig does with a receiver that runs it. The
16- and 17-byte objects are still unmeasured, contention is still unmeasured, and
`AP-BOOTSTRAP-1` remains unspecified.
