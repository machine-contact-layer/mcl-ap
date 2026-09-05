# Evidence: bit-error structure, 2026-09-05

Offline analysis of captures already retained by Experiments 003 and 008. **No
new transmission, no new hardware, no new claim about a link.** The captures are
unchanged; this directory holds only what the analysis printed.

## Files

| File | Cell | Payload |
|---|---|---|
| `003-dfr1154-11byte.txt` | Experiment 003, board→board | `00 02 00 00 00 01 01 00 00 01 3C` |
| `003-board-to-laptop-11byte.txt` | Experiment 003, board speaker → laptop mic | same |
| `008-wire-10byte.txt` | Experiment 008, board→host, Wire | `10 02 0D FB 11 54 00 00 04 3C` |
| `008-frame-24byte.txt` | Experiment 008, board→host, Link frame | `10 14 0D FB 11 54 00 01 00 0A 10 02 0D FB 11 54 00 00 04 3C 44 8D A4 5F` |

Ground truth is exact. Each payload is the hex the campaign's own run log
records as sent, not a reconstruction.

## Tool

`../../error_structure.c`, built as described in `../../README.md` §8. It
instruments the shipped decoder by including `src/ap_modem.c`; it does not
reimplement it.

The two `008` files were produced with `--sweep`, which re-decodes over a fine
grid of symbol rate and phase and reports the best error count reachable.
**`--sweep` scores against the known payload and is therefore a diagnostic, not
a receiver.** Its numbers are upper bounds on what better timing recovery could
achieve, never achievable rates.

The two `003` files were produced without `--sweep`.

## Headline numbers

```text
10 bytes   0.192% BER   9/10 clean   every error in the second half
11 bytes   0.357% BER   9/10 clean   every error in the second half
24 bytes   3.889% BER   3/10 clean   second half 3.9x worse than first
```

2.4x the payload, 20x the bit error rate. Errors are isolated rather than
bursty, asymmetric 77-100% in one direction, and low-margin in every cell.

Interpretation, and what it does not support, is in `../../README.md` §4-§7.
