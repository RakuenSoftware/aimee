# INVALID — abandoned E2B UD-Q4_K_XL 10k arm, 4 processes

Superseded by the rerun of the same arm at **3 processes**. Retained, not
deleted, because it is raw reporting output.

## Why it was abandoned

Two reasons, and the second is the one that matters.

1. **The host was oversubscribed.** Four llama-server processes at ~6.8 GB RSS
   each on a 30 GB host left ~3 GB free, and the kernel killed a server twice
   mid-arm — port 8403 at ~13:40Z and port 8400 at ~14:35Z. Neither logged an
   error; each log simply stops mid-task. Eight notes errored after exhausting
   their six client retries: g003515, g003519, g003523, g003527, g003531,
   g003535 (shard 3), and g005424 and g005428 (shard 0). The last of those is a
   503 rather than a connection reset — it arrived while the restarted server
   was still loading weights, which is a distinct failure and is why the retry
   budget should outlast a model load.

2. **Four processes is not comparable to the banked E4B arms**, which all ran at
   three (`procs=3` in each DONE line). The noise-floor experiment of the same
   day measured the process-count effect directly: one process against three
   moves 349 of 1001 notes and 0.0105 strict F1 — larger than any quant effect
   this ladder is trying to resolve. An E2B ladder at 4 could not have been
   compared to the E4B ladder at 3.

The driver comment justified 4 on the grounds that it "fits with room to
spare". That was a VRAM argument on a host whose limit turned out to be system
RAM, and it was never measured.

## Disposition

- 5,535 of 10,000 rows, of which 8 errored. Nothing here is scored or cited.
- The arm was stopped deliberately; it was not lost.
- Rerun: same label, same corpus (`data/corpora/v5/gold_large.jsonl`), same
  quant, NPROC=3.
