# SUPERSEDED — E2B UD-Q4_K_XL 10k, partial at --cache-ram 512

Stopped deliberately after ~1,089 rows, ~25 minutes in. Not a failure: zero
errored rows, memory healthy (6.75 GB across three servers), throughput at the
69 notes/min baseline.

Superseded only because `--cache-ram` was raised 512 -> 1024 MiB. At 512 the
server logged 89 prompt-cache evictions in ten minutes and prompt eval alternated
between 28 tokens (prefix hit) and ~515 tokens (miss, prefix recomputed). 1024
roughly halves that at a cost of +512 MiB per process, which the host has:
available RAM after the 512 fix was 22.4 GB.

`--cache-ram` is results-affecting — cache reuse decides whether a prefix is
restored or recomputed, the warm-server effect at 14/20 notes — so arms at 512
and 1024 cannot be mixed. Changed now rather than after Q4 completed, so that no
banked arm carries the interim value.

Superseded by: same label, same corpus, NPROC=3, CACHE_RAM_MIB=1024.
