# SUPERSEDED — E4B 10k arms measured at --cache-ram 8192 (the default)

Three complete, clean arms: Q4 0.6324, Q6 0.6450, Q8 0.6321, 10,000 rows each,
zero errored. Nothing wrong with them as measurements.

Superseded because `--cache-ram` is results-affecting and the E2B ladder now
runs at 1024 (finding 20). Cache reuse decides whether a prefix is restored or
recomputed, and those paths do not produce bit-identical logits -- the
warm-server effect, 14/20 notes. Arms compared to each other must share the
value, exactly like NPROC.

These remain valid for within-E4B comparison at 8192. They are not comparable to
any arm at 1024. Re-run under the same label at cache-ram 1024.
