# MCL-AP conformance

How an `AP-BOOTSTRAP-1` receiver is tested, and by what.

| Path | What it is |
|---|---|
| `vectors/` | PCM test vectors. **Generated** by `make_vectors.c`; never edited by hand. `vectors/VECTORS.md` is the manifest and is generated with them. |
| `make_vectors.c` | The generator. Includes `src/ap_modem.c` to reach `modulate()`, because a deliberately wrong CRC cannot be produced through the public encode API — which computes it correctly, as it should. |
| `independent/ap_bootstrap_rx.py` | A receiver written from `spec/ap-bootstrap-1.md` alone, sharing no code with `mcl-ap`. |
| `check-vectors.sh` | Runs both receivers over the corpus against one set of expectations. In the release rehearsal. |

## Why the vectors are binary, and why they are here rather than under `evidence/`

`evidence/` and `experiments/` hold **measurements** — what happened when a
signal crossed a room. These are not that. They are the **agreed bytes** two
implementations are compared against, generated deterministically from a
tracked tool, and a specification that requires a receiver to decode particular
samples cannot ship without those samples. They cannot be text.

`check-provenance.sh` accounts for all three locations on that basis, and the
requirement it enforces is unchanged: every binary in the tree sits somewhere a
README says what produced it.

## What passing this proves, and what it does not

Both receivers agree on all nine vectors in both directions, and the three
positive payloads come back byte-identical. That establishes the specification
is **self-sufficient**: no parameter a receiver needs lives only in the
reference implementation.

It does **not** establish that the specification is unambiguous to a reader who
has never seen the reference, because the same author wrote both. §11 of the
profile says so, and this file repeats it, because a cross-test that looks like
independent verification is exactly the kind of result that gets quoted as more
than it is.

It also does not establish that a receiver works over air. **These vectors are
noise-free** — they are the transmitter's own output — so a receiver that
omitted the mandatory whole-frame rate refinement of §6.4 would pass every one
of them. Timing recovery is what fails in a room, and these have perfect
timing. The over-air evidence is `experiments/011-bootstrap-over-air/`, and the
two are not substitutes for each other.

## Regenerating

```sh
cmake -S . -B build -DMCL_AP_BUILD_EXPERIMENTS=ON
cmake --build build --target mcl_ap_make_vectors
./build/mcl_ap_make_vectors conformance/vectors
sh conformance/check-vectors.sh
```

Regenerate deliberately. A vector regenerated to make a failing cross-test pass
has destroyed the only thing it was for.
