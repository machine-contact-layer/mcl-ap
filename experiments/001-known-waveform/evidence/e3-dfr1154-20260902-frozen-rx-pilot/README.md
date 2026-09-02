# E3 pilot — 3 trials (DFR1154, frozen receiver)

**Date**: 2026-09-02
**Result**: 3 / 3 recovered the exact MCL Wire bytes and the exact PRESENCE semantic object.

Three-trial pilot taken immediately before the ten-trial set in
`../e3-dfr1154-20260902-frozen-rx/`, under identical conditions and against the same frozen
receiver. Retained so the full run is on record rather than only the set that was planned in
advance.

| Trial | Preamble acquired | Correlation | CRC valid | Wire byte-exact | Semantic exact |
|-------|-------------------|-------------|-----------|-----------------|----------------|
| 1 | yes | 0.876223 | yes | yes | yes |
| 2 | yes | 0.877340 | yes | yes | yes |
| 3 | yes | 0.875073 | yes | yes | yes |

Combined with the ten-trial set: **13 trials, 13/13 acquisition, 11/13 exact recovery.**
See the main set's README for conditions and for the characterization of the two failures.
