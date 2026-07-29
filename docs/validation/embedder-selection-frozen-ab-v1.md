# Embedder measurements — frozen-ab-v1 (2026-07-29)

> **STATUS: MEASUREMENTS COMPLETE, DECISION OPEN.** This page records what was
> measured. It does **not** record a settled embedder choice: the leading option
> depends on whether query/document prefix support is built, and that is an open
> decision. See [The prefix fork](#the-prefix-fork).
>
> An earlier version of this page declared nomic-embed-text-v2-moe adopted. That
> declaration was premature — it selected on a prefixed benchmark score while the
> code ships prefix-free. The measurements below are sound; the conclusion drawn
> from them was not.

## Environment

- **Suite:** `eval/frozen-ab-v1` (`aimee-encoder`), manifest SHA-256
  `16d2c16add86052ff24be410699ab9452ee1a36252de6dba31ab5391de7ab81c`.
  10,000 cases ranked against all 26,473 corpus documents. The corpus is **not**
  capped — capping inflates NDCG and is what produced the withdrawn SciFact
  numbers in [embedder-gate-scifact](embedder-gate-scifact.md).
- **Harness:** `scripts/eval_hf_embedder.py`, unmodified, except a `--pooling last`
  option added for Qwen3 (validated: `padding_side=right`, index `mask.sum(1)-1`
  resolves to the trailing `<|endoftext|>`, which is Qwen3-Embedding's readout).
- **Hardware:** RTX 5080, bf16. CPU throughput separately on 16 pinned threads.
- **aimee commit:** `a8d3214c`.

## Results — every model at its best

Each model with its **own card-recommended prefix and native pooling**. This is
the correct way to benchmark, and it is what the original sweep did.

| model | NDCG@10 | R@10 | dim | **code** | prose | cited | vec/s (GPU) |
|---|---:|---:|---:|---:|---:|---:|---:|
| **nomic-embed-text-v2-moe** | **0.6072** | 0.8007 | 768 | **0.8104** | 0.5157 | 0.6344 | 82.7 |
| **Qwen3-Embedding-4B** | 0.6061 | **0.8100** | 2560 | 0.7394 | **0.5274** | 0.6988 | 26.4 |
| **bekko-embedding-v1-a25m** | 0.5909 | 0.7816 | 384 | 0.7718 | 0.4841 | **0.7170** | **510.7** |
| **Qwen3-Embedding-0.6B** | 0.5810 | 0.7765 | 1024 | 0.7325 | 0.4930 | 0.6804 | 113.1 |

Reproduction check: nomic re-measured at **0.6072** against a recorded 0.6058, and
a25m at **0.5909** against 0.5892 — both within GPU noise. The suite is stable.

### What the table says

- **nomic and Qwen3-4B are tied** (0.6072 vs 0.6061 — a 0.0011 difference is
  noise). 4B costs **3.3× the vector storage** and is **3.1× slower to embed**
  for that tie.
- **The Qwen3 ladder tops out at parity.** 0.6B → 4B is +0.025 for 6.7× the
  parameters, arriving where a 475M model already sits. Scaling the family is a
  more expensive route to the same score, not an upgrade path.
- **nomic leads decisively on code** (+0.071 over 4B, +0.039 over a25m). aimee
  embeds code as a first-class citizen (`code_embeddings`, and `intent_vec` /
  `sig_vec` / `body_vec` per code unit), so this category is weighted heavily for
  this workload.
- **a25m is far the cheapest** — 510.7 vec/s on GPU (6.2× nomic, 19× 4B) and
  2,155 tok/s on CPU.

## The prefix fork

**The benchmark applies per-model prefixes. aimee has no prefix plumbing.** So
for a prefix-dependent model, the benchmark score is *not* the deployed score:

| model | with card prefix | prefix-free (what aimee serves today) | delta |
|---|---:|---:|---:|
| nomic-embed-text-v2-moe | 0.6072 | 0.5823 | **−0.0249** |
| Qwen3-Embedding-0.6B | 0.5810 | 0.5275 | **−0.0535** |
| bekko-a25m | 0.5909 | **0.5909** | none — its card defines no prefix |

This inverts the ranking. **On paper nomic leads; as the system is built today,
a25m leads.** Two coherent positions follow, and the model choice is downstream
of which is taken:

1. **Build per-model prefix support** → nomic at 0.6072 is the best measured
   option, with the largest code-retrieval margin.
2. **Do not** → a25m at 0.5909 is the best option, and it needs no new machinery,
   no prefix, no pooling special-case.

Selecting on column one and serving column two is the incoherent state, and it is
what the current code does. See
[embedder-query-document-prefixes](../proposals/pending/embedder-query-document-prefixes.md).

Note prefix support must be **per model**, not a global setting: a25m takes none,
nomic takes `search_query:`/`search_document:`, Qwen3 takes an instruction
sentence containing a newline. Pooling is likewise per model (`mean` vs `last`).

## Serving verification (nomic, llama.cpp)

Verified end-to-end, not inferred:

- GGUF sha256 matches the pinned digest; `general.architecture = nomic-bert-moe`,
  registered in the already-pinned `LLAMA_TAG=b9775` — no runtime bump needed.
- Serves at **768-d, mean pooling, L2-normalised**, on both `-ngl 0` and `-ngl 99`.
- Q8_0 vs bf16 agree at **cosine 0.999** uniformly across all length buckets
  (0–2048 tokens), and cost only **−0.0037 NDCG** end-to-end. Q8_0 is fine.
- `max_trained_positions = 2048` confirms 2048-token truncation is legitimate;
  the tokenizer's `model_max_length: 512` is a tokenizer artifact, not a limit.

### CPU throughput — corrected

| runtime | tok/s |
|---|---:|
| torch fp32 (the number the original decision used) | 787 |
| **llama.cpp Q8_0, GPU made invisible** | **598** |

nomic is **3.6× slower than a25m on CPU** (598 vs 2,155), not the 2.7× recorded.
llama.cpp Q8_0 is *slower* than torch fp32 here, which is plausible for an MoE on
CPU. An intermediate measurement of 4,278 tok/s was **GPU-contaminated** (`-ngl 0`
is overridable by llama.cpp's auto-fit) and is withdrawn.

## Corrections to earlier claims on this page

- ~~"Uniform 768-d across tiers is an architectural win."~~ **Overstated.** aimee
  already handles per-deployment dimensions (`db2_embedding_dim_record_or_check`,
  `db2_effective_dim`, `db2_dim_change_reset`), and `EMBED_MAX_DIM = 4000` with
  the 8B's 4096→4000 truncation exists specifically to support a mixed ladder.
  Uniformity saves a re-embed when switching tiers — a convenience, not a
  correctness property.
- ~~"The suite was scored prefix-free for all candidates."~~ **False.** Each model
  received its card prefix.
- ~~"2.7× slower on CPU."~~ **3.6×**, see above.

## Caveats

- Embedder-isolated retrieval. The full aimee pipeline adds reranking and fusion,
  which can reorder these results.
- **a25m cannot be baseline-gated.** It is ten days old with no published MTEB
  numbers, so unlike nomic and Qwen3 there is no external result to check our
  harness against. It did reproduce across two independent runs here, and the
  suite is built from aimee's own corpus, but the external cross-check that
  `embedder-gate-scifact` treats as standard practice is unavailable.
- a25m's multilingual capability — its headline feature — is unmeasured by this
  suite.
- Qwen3 was run with its card's generic *"web search query"* instruction; a
  corpus-tuned instruction was not tested. Qwen3-8B was not measured (prior
  aimee-code data showed 4B→8B is +0.16, i.e. noise).
