# Qwen3-Reranker latency on 7900XTX (RADV/Vulkan) — 2026-06-22

Per-query rerank latency = K full forward passes over (query, doc). SciFact text
docs (~320 tok each), single batched request. **Native `/v1/rerank`** is the
OPTIMAL path (one forward pass + pooling, no generation):

| model | ms/candidate | K=5 | K=10 | K=20 | K for ~1s |
|---|---|---|---|---|---|
| 0.6B | ~130 | 0.7s | 1.4s | 2.7s | ~7 |
| 4B   | ~187 | 0.94s | 1.8s | 3.7s | ~5 |
| 8B   | ~264 | 1.3s | 2.6s | 5.4s | ~4 |

**BUT native `/v1/rerank` scores Qwen3-Reranker WRONG** (toy gate: irrelevant doc
scored highest — llama.cpp's classifier-head rerank path is invalid for this
yes/no causal model). Correct scoring needs the **yes/no-logit path** (format the
reranker template, generate 1 token, softmax P(yes) vs P(no)) — which is ~2.3×
slower (0.6B K=10: 3.2s via /completion vs 1.4s native):

| model (yes/no path) | ms/cand | K=10 | K=20 | K=50 | K for ~1s |
|---|---|---|---|---|---|
| 0.6B | ~320 | 3.2s | 4.4s | 9.5s | ~3 |

Code docs (longer, ~500-2000 tok) are slower still.

## Verdict
No Qwen3-Reranker tier reranks a useful candidate set (top-20–100) in <1s on this
GPU. Correct-scoring 0.6B fits only ~3 candidates in 1s; native-path best case ~7.
Reranking top-20 costs 3–4s; top-100 costs 12–16s. **Reranking cannot be a
synchronous real-time step here for meaningful depth.**

Quality (for context): SciFact 0.6B reranker lifts first-stage nDCG@10 0.7059 ->
0.7598 (+5.4) at top-100 — real quality, but seconds of latency.

## Architecture comparison (SciFact, first-stage Qwen3-0.6B embed = 0.7059)

| reranker | arch | quality (nDCG@10) | uplift | latency K=20 | correct via |
|---|---|---|---|---|---|
| Qwen3-Reranker-0.6B | generative cross-encoder | 0.7598 | +0.054 | 2.7s native / 4.4s correct | yes/no logits (native /v1/rerank BROKEN) |
| bge-reranker-v2-m3 (568M) | BERT cross-encoder | 0.7392 | +0.033 | **0.79s** | native /v1/rerank (correct) |

bge native latency: K=5 179ms, K=10 343ms, K=20 791ms (~36 ms/cand) — ~4x faster
than Qwen3-0.6B native, ~9x faster than Qwen3's correct yes/no path, and it fits
top-20 in <1s. Quality is ~60% of Qwen3-0.6B's uplift. **bge is the real-time
reranker; Qwen3-Reranker is async-only here.**

## Quality (SciFact, first-stage Qwen3-0.6B embed nDCG@10 = 0.7059)
| reranker | uplift topk20 | uplift topk100 | toy gate |
|---|---|---|---|
| Qwen3-Reranker-0.6B | +0.037 | +0.054 | PASS |
| Qwen3-Reranker-4B   | +0.009 (anomalous) | — | PASS |
| bge-reranker-v2-m3  | +0.033 | — | PASS |

4B's low topk20 uplift despite a passing toy gate = score saturation (Qwen3-Reranker
piles scores at [0.99,1]/[0,0.01]; at shallow depth ties dominate). Published MTEB-R
(top-100, many datasets) has 4B 69.76 > 8B 69.02 > 0.6B 65.80, so 4B≥0.6B in general;
the SciFact-topk20 number is not representative. A faithful 4B/8B quality read needs
the full top-100 protocol.

## Architecture verdict (real-time <1s, 7900XTX)
- **Qwen3-Reranker (generative cross-encoder)**: best published quality, but (a) too
  slow — correct yes/no path is ~320 ms/cand (0.6B), top-20 = 4.4s; (b) llama.cpp
  native /v1/rerank scores it WRONG; (c) saturates at shallow depth. => async-only.
- **bge-reranker-v2-m3 (BERT cross-encoder, 568M)**: correct via fast native
  /v1/rerank, ~36 ms/cand, top-20 = 0.79s (<1s), +0.033 uplift. => the real-time pick.
- **ColBERT / late-interaction (not benchmarked here)**: doc token-embeddings
  precomputed at ingest, so query-time latency is ~one query-encode + maxsim,
  INDEPENDENT of K (~tens of ms for any depth). Highest throughput; needs token-
  embedding storage and a maxsim path (no llama.cpp rerank endpoint). Quality
  typically sits between bi-encoder and full cross-encoder.

## DECISION (2026-06-22): Ettin ModernBERT rerankers (replaces Qwen3-Reranker)
Modern (2025) ModernBERT encoders — correct + fast via native /v1/rerank (toy gate
passed, well-spread scores, NOT saturated like Qwen3). Already aimee's baseline
family ("pplx/ettin"). Per published MTEB(eng,v2): ettin-32m 0.578 > bge-v2-m3
0.553; ettin-150m 0.599 > Qwen3-Reranker-0.6B 0.594; ettin-1b 0.611 = mxbai-large.

Measured on 7900XTX (SciFact docs ~320 tok, native /v1/rerank):
| model | GPU ms/cand | GPU top-20 | CPU ms/cand (-fa on,16T) | CPU top-10 | CPU top-20 |
|---|---|---|---|---|---|
| ettin-400m | ~36 | 0.76s | ~470 (no-fa) | — | 9.5s |
| ettin-68m  | (~6-7) | <0.2s est | **~60** | **0.60s** | 1.22s |

**GPU reranker = ettin-400m** (top-20 in 0.76s, quality ~0.605). **CPU reranker =
ettin-68m** with -fa on (top-10-15 in <1s; quality ~0.589 > bge). Qwen3-Reranker
REJECTED for the real-time path (generative yes/no, ~320ms/cand correct path,
top-20=4.4s, native /v1/rerank scores it WRONG). -fa flag: was 'auto' originally;
forcing 'on' is a minor win on short rerank pairs (FA helps less at ~320 tok / on CPU).

## FASTPATH (-fa on) — final numbers
ettin-400m GPU: -fa on = 17 ms/cand (top-20 0.34s) vs -fa auto 36 ms/cand (0.76s).
**Flash Attention ~2x on GPU; 'auto' did NOT engage it on this Vulkan build — force -fa on.**
ettin-68m CPU -fa on = 60 ms/cand (top-10 0.60s). FA is a minor win on CPU.
FINAL: GPU reranker ettin-400m -fa on (top-20 0.34s); CPU reranker ettin-68m -fa on (top-10 <1s).
