# INVALID — E2B UD-Q4_K_XL 10k, abandoned at cache-ram 8192 (default)

4,180 clean rows, zero errored. Stopped deliberately, not lost.

## Why

`llama-server --cache-ram` defaults to **8192 MiB per process**, and no arm in
this campaign ever set it. Three servers therefore reserved 24 GiB of host RAM
for prompt cache on a 31.4 GiB box, and four reserved 32 GiB — more than the
machine has. That, not VRAM, is what killed two servers mid-arm earlier the same
day with nothing in their logs but a truncated line.

The evidence is unambiguous: per server `RssAnon` 7.4 GB against `RssFile` 158 MB
(`-ngl 99` keeps the weights on the card), swap fully consumed, and the server
log repeating `alloc: - making room for prompt cache entry, removing oldest
entry (size = 27.482 MiB)`.

## Why the arm was restarted rather than finished

`--cache-ram` changes results. Whether a prefix is restored from cache or
recomputed decides the logits, which is the warm-server effect already measured
in this campaign at 14/20 notes. It is a configuration variable exactly like
NPROC, so an arm at 8192 cannot be compared with an arm at 512. Finishing this
one would have made the E2B ladder internally inconsistent, which is worse than
the cost of restarting it.

## Consequence that is NOT resolved

The three banked E4B 10k arms ran at the 8192 default. The E2B ladder now runs
at 512. **Cross-family comparison at 10k is therefore invalid until the E4B arms
are re-run at 512.** Within-family comparisons on each side remain valid.

Superseded by: same label, same corpus, NPROC=3, CACHE_RAM_MIB=512.
