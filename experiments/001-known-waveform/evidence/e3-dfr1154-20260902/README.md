# DFR1154 development captures (E2 — replay evidence only)

**Date**: 2026-09-02
**Evidence level**: E2 (recorded-channel replay). **Not E3.**

The three captures here were taken while the DFR1154 USB capture instrument was still being
brought up. They are retained because they document how the transport and the receiver were
debugged, and because `capture-005` is the artifact on which the symbol-boundary rounding
change was diagnosed.

| File | Status |
|------|--------|
| `capture-003.wav` | board mic clearly recorded the chirp and FSK burst, but Windows playback latency put the preamble outside the decoder's fixed acquisition window |
| `capture-004-synchronized.wav` | playback synchronization corrected; later rejected as transport-corrupted after the USB CDC driver was found to silently drop 56 payload bytes |
| `capture-005-crc-verified.wav` | first capture with the repaired transport: full 288,044 bytes, board CRC-32 `DF6E6D65` verified host-side |

## Why capture-005 is E2 and not E3

`capture-005` decodes to the exact Wire bytes `00 02 00 00 00 01 01 00 00 01 3C` and the exact
PRESENCE object. But it only does so through a receiver that was changed *after* the capture
was recorded: symbol boundaries are now rounded to nearest rather than truncated. A stored
capture that decodes because the decoder changed is replay evidence, not a controlled over-air
result.

That change was diagnosed here, on this file, so this file cannot also serve as its
confirmation. Measured on the swept phase/rate grid for this capture, rounding raised the
number of clean operating points from 98 to 103 of 205, and at the estimator's own operating
point (phase 0, 159.90 samples/symbol) it moved the frame from one bit error at −0.0412 margin
to zero errors at +0.2783. That is a real but modest improvement; it is not the reason E3
succeeded.

The E3 result is in `../e3-dfr1154-20260902-frozen-rx/`, taken afterwards against the frozen
receiver.
