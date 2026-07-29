# Embedder selection — frozen-ab-v1 (nomic-embed-text-v2-moe)

**Decision: `nomic-embed-text-v2-moe` is the base embedder on EVERY tier, at a
uniform 768 dimensions.** It replaces the Qwen3-Embedding ladder (0.6B/1024-d on
CPU, 4B/2560-d on GPU). Adopted 2026-07-29.

This supersedes [embedder-gate-scifact](embedder-gate-scifact.md) and
[embedder-gate-locomo](embedder-gate-locomo.md) as the standing embedder
decision. Both are retained as evidence.

## Environment and commands

- **Suite:** `eval/frozen-ab-v1` in the `aimee-encoder` repository, manifest
  SHA-256 `16d2c16add86052ff24be410699ab9452ee1a36252de6dba31ab5391de7ab81c`.
  10,000 embedding cases ranked against all 26,473 corpus documents.
- **Environment:** CT 106 (`aimee-train`) on `.253`. Accuracy on an RTX 5080
  (bf16); CPU throughput on 16 pinned threads (fp32). torch 2.11.0+cu128,
  transformers 5.14.1, tokenizers 0.22.2.
- **aimee commit at integration:** `a8d3214c`.

```bash
# accuracy (GPU, full corpus)
python scripts/eval_hf_embedder.py --model nomic-ai/nomic-embed-text-v2-moe \
  --pooling mean --suite eval/frozen-ab-v1 --device cuda --max-tokens 2048 \
  --trust-remote-code --output final-nomic.json

# CPU throughput
python scripts/bench_cpu_throughput.py --model nomic-ai/nomic-embed-text-v2-moe \
  --pooling mean --corpus eval/frozen-ab-v1/corpus.jsonl \
  --texts 128 --latency-samples 12 --threads 16 --trust-remote-code
```

## Results

14 candidates were measured. The decision came down to two:

| | nomic-embed-text-v2-moe | bekko-embedding-v1-a25m |
|---|---:|---:|
| **NDCG@10** | **0.6058** | 0.5892 |
| MRR@10 | 0.5420 | — |
| Recall@1 / @5 / @10 | 0.3833 / 0.7381 / **0.8007** | — / — / 0.7816 |
| `code_unit_body` | **0.8086** | 0.7701 |
| `prose` (60% of queries) | **0.5146** | 0.4819 |
| `cited_artifacts` | 0.6325 | **0.7170** |
| Output width | **768** | 384 |
| Parameters | 475M | **123M** |
| CPU throughput | 787 tok/s | **2,155 tok/s** |
| Licence | Apache-2.0 | MIT |

nomic wins overall, on code, and on the prose bucket that is 60% of the query
mix. **a25m wins `cited_artifacts` decisively** and is 2.7× faster on CPU.

### The CPU cost was accepted deliberately

787 tok/s (p50 967 ms per document, 16 threads) is 2.7× slower than a25m. That
number was measured *before* the decision was confirmed, not discovered after
it. It buys +2.8% NDCG, the two categories that dominate the query mix, and
double the dimensional headroom against a corpus heading toward millions of
documents — where 384 dimensions were expected to crowd, and could not be
widened because a25m's 384 is native rather than a Matryoshka truncation.

## What this changes architecturally

**The embedding dimension is now uniform at 768 across every tier.** Previously
the CPU tier ran 1024-d and the GPU tiers 2560-d, so an index built under one
tier was unreadable under another and moving between them was a drop-and-rebuild
re-embed. Tier selection is now a pure speed decision: the same GGUF, the same
768-d vectors, differing only in GPU offload.

Because of that, `AIMEE_EMBEDDING_DIM` is no longer set per-tier in the deploy
files. It is left unset so the kb derives the dim (**pinned > recorded > probed >
default**); setting it counts as an operator pin and suppresses that derivation.

## Serving

No new runtime. It serves through the existing llama.cpp Vulkan container:

| | |
|---|---|
| Repo / file | `ggml-org/Nomic-Embed-Text-V2-GGUF` / `nomic-embed-text-v2-moe-q8_0.gguf` |
| Revision | `498da4a128ed12a423efb6f9b0242dbac80209bf` |
| SHA-256 | `36c5817bc25f379e62021f49efde05b10ed3b0c93ab8059c43173a7a5de73565` |
| Architecture | `nomic-bert-moe` |
| Pooling | **mean** |

Two facts were verified rather than assumed before integration:

- The GGUF header declares `general.architecture = nomic-bert-moe`, and
  llama.cpp at the already-pinned `LLAMA_TAG=b9775` registers
  `LLM_ARCH_NOMIC_BERT_MOE` (`src/llama-arch.cpp`). **No runtime bump is
  required.**
- The sentence-transformers pipeline is `Transformer → Pooling(mean) →
  Normalize` with **no Dense head**, so a straight GGUF conversion captures the
  whole model. (Contrast the Ettin reranker, whose score head does not survive
  conversion and is shipped separately as `head.npz`.)

> **Pooling is not cosmetic.** nomic declares mean pooling. Serving it with
> `last` — correct for the previous Qwen3 embedder and the prior default of
> `AIMEE_LLM_EMBED_POOLING` — yields vectors that are wrong but well-formed:
> nothing errors, the dimension still checks out, and retrieval quality silently
> collapses. The default is now `mean`.

## Migration

Existing deployments are **not** migrated implicitly, and the cutover is
fail-closed by construction. A populated kb keeps its recorded
`kb_meta.schema_embedding_dim`, which outranks the new 768 default, so nothing
silently re-points.

Two independent guards sit in front of a corpus built by the old embedder:

- **Dimension guard** (`db2_embedding_dim_record_or_check`). Fires for every
  deployment: a corpus recorded at 1024 or 2560 refuses a 768 embedder. This is
  the one that will actually trigger in practice.
- **Model-identity guard** (`db2_embedding_model_record_or_check`). Catches a
  *same-dim* model swap, which the dimension guard cannot see. It is driven by
  the kb-side `embedding_model` config, which **defaults to empty** — an empty
  identity makes the guard a deliberate no-op, so it only engages for operators
  who set it. Note this is a different value from the gateway's
  `AIMEE_LLM_EMBED_MODEL`, which serves `/health` and does not feed the guard.

The deliberate path through both is the already-shipped dim-change reset —
`aimee kb reembed` / `db2_dim_change_reset`, double-gated by
`kb_reembed_on_dim_change` plus `--confirm`. It drops and recreates only the
*derived* vector tables and re-queues their authoritative sources, so no source
data is lost. See [unified-llm-cutover](../runbooks/unified-llm-cutover.md).

Operators who pinned `embedding_model` to a Qwen3 identity must update it as part
of the same maintenance window, or the identity guard will refuse after the
re-embed.

## Caveats — do not over-read

- This is **embedder-isolated** retrieval. The full aimee pipeline adds reranking
  and fusion, which can reorder these results. It is not the production cutover
  number.
- a25m beats nomic on `cited_artifacts` (0.7170 vs 0.6325). The decision trades
  that category away for code and prose.
- **The suite was scored prefix-free for all 14 candidates.** That is uniform
  treatment but *not* neutral treatment — models differ in how much they depend
  on their prefixes, so prefix-dependent models are understated by an unequal
  amount. nomic specifies `search_query:` / `search_document:` and won anyway, so
  0.6058 is a floor rather than a ceiling; the same is true to an unknown degree
  for other candidates, so the **ranking carries a caveat even though the
  treatment was even**. See
  [embedder-query-document-prefixes](../proposals/pending/embedder-query-document-prefixes.md).
- Q8_0 quantisation is used in serving, while the selection score was measured at
  bf16 on GPU. Prior work found Q8 lossless for embedding on a comparable model
  (4B-Q8 NDCG == 4B-f16), but that has **not** been re-measured for nomic.
  Validation-pending.
