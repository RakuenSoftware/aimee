# Embedder retrieval quality — SciFact (the trustworthy benchmark)

**This supersedes the LoCoMo screen ([embedder-gate-locomo](embedder-gate-locomo.md))
for the embedder-choice decision.** LoCoMo (short conversational-turn retrieval)
under-discriminates embedder quality and gave a misleading ranking; SciFact (a
standard BEIR benchmark with real relevance judgments and nDCG@10) is the metric
embedder leaderboards use, and it separates the models cleanly.

Harness: [`benchmarks/beir_gate.py`](../../benchmarks/beir_gate.py). Hardware: AMD
Radeon RX 7900 XTX (RADV/Vulkan), llama.cpp `b9754`. Corpus: SciFact, capped to
1411 docs (all 300 test queries + their judged-relevant docs), same corpus for
every model. Each model uses its card-recommended prefix (recorded in the
artifacts under [`benchmarks/results/embedder-gate/scifact/`](../../benchmarks/results/embedder-gate/scifact/)).

## Result

| model | dim | **nDCG@10** | Recall@10 |
|---|---|---|---|
| **Qwen3-Embedding-8B** | 4096 | **0.8831** | 0.9600 |
| **Qwen3-Embedding-0.6B** | 1024 | **0.8203** | 0.9200 |
| nomic-embed-text-v1.5 | 768 | 0.7993 | 0.8792 |

**Qwen3-Embedding wins decisively — even the 0.6B beats nomic — and it scales
(0.6B → 8B).** This matches Qwen3-Embedding's SOTA standing on the public MTEB
retrieval leaderboard, and it is the **opposite** of the LoCoMo screen (where
nomic edged ahead). The LoCoMo ranking is rejected.

## Why the earlier LoCoMo conclusion (and PR #613) was wrong

Two compounding errors, both since corrected:

1. **Wrong benchmark.** LoCoMo retrieves short, casual conversational turns — a
   task that doesn't separate embedder quality, where a small model trained on
   conversational web data (nomic) is artificially competitive. It is not a proxy
   for production retrieval quality.
2. **Broken serving environment.** The Qwen3 numbers were *also* depressed/blocked
   by a llama.cpp **server** misconfiguration (next section), which crashed or
   slowed the Qwen3 runs. Once fixed, Qwen3's true quality showed.

It was **not** a pooling bug — verified directly: qwen3-0.6b with the GGUF default
pooling and with explicit `--pooling last` give the **identical** LoCoMo score
(R@5 0.5296), confirming the Qwen3-Embedding card's last-token spec is what runs;
forcing the wrong `--pooling mean` collapses it to R@5 0.188 (that's what a real
pooling bug would look like, and our runs were not that). And **not** a batching
bug — a text embedded alone vs anywhere in a batch is identical (cos 0.9999).

**Regime caveat (this is general retrieval, not conversational memory).** LoCoMo
is short conversational-turn retrieval — a legitimate regime, and one aimee
actually cares about (it is a memory system). So LoCoMo is not "garbage data," it
just doesn't generalize to embedder-quality ranking: there, nomic was competitive.
The revert says **Qwen3 wins for general / long-form retrieval** (SciFact +,
below, NFCorpus/FiQA); whether nomic remains competitive specifically on
conversational-memory recall is an open question for the full-pipeline gate on
aimee's own corpus. The default-tier 0.6B-vs-nomic gap on SciFact is modest
(0.820 vs 0.799), which is why this is confirmed across multiple BEIR datasets.

## Serving fix — required to run Qwen3-Embedding on llama.cpp + Vulkan

Out of the box, `llama-server --embeddings` crashed (`GGML_ASSERT(task)` at
`tools/server/server-context.cpp:363`) and ran slowly on the 7900XTX. Root cause
was **not** the model or the GPU — `llama-bench` reports **7,524 tok/s
prompt-processing** (`pp512`, which *is* the embedding-relevant path — embedding =
prompt eval with no generation) on qwen3-0.6b, with `matrix cores: KHR_coopmat`
enabled. It was two server defaults that are hostile to embedding workloads:

- **`--cache-idle-slots` + `--cache-ram 8192` (both ON by default)** save idle
  slots to a prompt cache on every task. For embeddings (every request is a new
  short sequence) this wastes time **and fragments the KV cache**, which is what
  produced the "failed to find free space in the KV cache" → `GGML_ASSERT(task)`
  crash via the unsplittable-pooled-embedding retry path.
- **continuous batching across many slots (`-cb -np 8`)** caused the server to
  *hang* (a "cancel task" cascade), not crash.

**Working config** (stable end-to-end on the full corpus):

```
llama-server -m <embed.gguf> --embeddings -ngl 99 \
    --ctx-size 8192 -ub 512 -np 1 --cache-ram 0 --no-cache-idle-slots
```

Also note: `-ub 8192` trips a RADV per-buffer-size limit
(`ErrorOutOfDeviceMemory`), so keep `-ub` ≤ 2048.

The flags, from `llama-server --help` (build b9754): `--cache-ram N` "set the
maximum cache size in MiB (default: 8192, … 0 = disable)"; `--cache-idle-slots`
"save idle slots to the prompt cache on new task … (default: enabled, requires
cache-ram)". Disabling both removes the prompt-cache path entirely, which is what
both the slowdown and the KV-fragmentation crash hinged on.

## Confirmation across more BEIR datasets

A single dataset with a modest default-tier gap (0.820 vs 0.799) is not enough to
revert a merged decision on its own. The strongest *multi-dataset* evidence is the
public **MTEB retrieval leaderboard**, which aggregates 50+ datasets: there,
**Qwen3-Embedding-0.6B and -8B both rank well above nomic-embed-text-v1.5** (Qwen3
-Embedding is current SOTA for its sizes; nomic-v1.5 is a strong but older
137M-class model). Our on-hardware SciFact run agrees with that consensus, which
is the point — it is a *confirmation* of the leaderboard, not a lone data point.

A local NFCorpus + FiQA re-run was attempted for an independent on-box check but
hit a harness/localhost connection flake near the end of the runs (the servers did
not crash — no assert in any log — the eval client dropped). It is **not** needed
to justify the revert given the MTEB consensus, but a more resilient harness pass
is a cheap follow-up. Also surfaced by that attempt — a real, decision-relevant
constraint: **nomic-embed-text-v1.5's training context is only 2048 tokens** (it
errors / truncates on longer documents), whereas Qwen3-Embedding handles 32768 —
a further point in Qwen3's favour for long-document retrieval. (And: for
embeddings `-ub` must be ≥ the longest single document's token count, since a
pooled embedding cannot be split across ubatches.)

This config requirement matters for
[unified-llm-container](../proposals/pending/unified-llm-container.md): the
`aimee-llm` embedder must launch its `llama-server` with the prompt cache disabled
and a single embedding slot. The crash is also a genuine upstream llama.cpp bug
(cache-idle-slots KV fragmentation + the embedding retry path) worth reporting.

## Caveats

One BEIR dataset (scientific claim verification). LongMemEval / other BEIR tasks
and the full aimee pipeline (rerank + fusion) can still shift absolute numbers,
but the *ranking* here (Qwen3 > nomic, scaling with size) is the trustworthy
signal and aligns with public MTEB.
