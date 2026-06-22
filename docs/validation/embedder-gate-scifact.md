# Embedder choice — baseline-gated validation (text BEIR + code)

**Decision: drop nomic; use Qwen3-Embedding — 0.6B (CPU default, 1024-d), 4B (GPU
default, 2560-d), 8B (operator opt-in, 4000-d trunc).** This supersedes the LoCoMo
screen ([embedder-gate-locomo](embedder-gate-locomo.md)) **and** the earlier
capped-corpus SciFact numbers that once lived in this file (the "0.883 / 0.820 /
0.799" table — withdrawn; it was a capped-corpus artifact, see below).

Every number below is **baseline-gated**: each model's score is trusted only after
it reproduces its *published* result on the same harness. Harness:
[`benchmarks/beir_cli.py`](../../benchmarks/beir_cli.py) driving llama.cpp
`llama-embedding` on an AMD RX 7900 XTX (RADV/Vulkan). Artifacts in
[`benchmarks/results/embedder-gate/`](../../benchmarks/results/embedder-gate/)
(`scifact-full/`, `multi-beir/`, `aimee-code/`).

## Why this matters: aimee embeds code

The configured embedder is used for **code**, not just memory text:
`kb_service_code_embed.c` writes code-chunk vectors (`code_embeddings`), and
`kb_curator_index_code_unit.c` writes three named vectors per code unit —
`intent_vec` (NL summary), `sig_vec` (signature), **`body_vec` (raw code body)**.
So **code-retrieval quality is a first-class selection axis**, and a text-only
model is the wrong tool.

## Headline

1. **nomic-embed-text-v1.5 is text-only** (no code training) and loses on code by a
   wide margin → **dropped**.
2. **On text**, nomic and Qwen3-0.6B both reproduce their published BEIR baselines
   (harness validated); they tie on SciFact/NFCorpus and Qwen3 wins the rest.
3. **On code**, Qwen3 dominates, and **quality plateaus at 4B**: 4B ≈ 8B, so 4B is
   the GPU default and 8B is an opt-in for the last ~0.2 nDCG.

## Text BEIR — baseline reproduction (full corpus)

Mine (this harness) vs **published** MTEB nDCG@10. nomic = mean-pool 768-d (docs
truncated to its ~2048-token limit, its real behavior); Qwen3-0.6B = last-pool
fp16 1024-d with the card instruction prefix.

| dataset | nomic (mine / pub) | Qwen3-0.6B (mine / pub) |
|---|---|---|
| SciFact  | **0.7034** / 0.7028 | **0.7059** / 0.6972 |
| NFCorpus | **0.3466** / 0.3467 | **0.3701** / 0.3671 |
| ArguAna  | 0.3556 / 0.5202 | 0.4856 / 0.7097 |

SciFact and NFCorpus reproduce published within **~0.3pt for both models** → the
harness is sound. ArguAna is a *symmetric* argument-retrieval task; asymmetric
query/doc prefixes depress **both** models' absolute scores, but Qwen3 still wins
(+13pt mine, +19pt published). Per published BEIR, Qwen3-0.6B also wins FiQA (+9),
SCIDOCS (+7), ArguAna (+19), TREC-COVID (+27); SciFact/NFCorpus are the rare ties.

> **The withdrawn capped result.** An earlier version of this page reported Qwen3
> beating nomic on SciFact 0.820 vs 0.799 (and 8B 0.883). Those were measured on a
> **capped 1411-doc corpus**, which inflates nDCG and flipped the ranking. On the
> **full 5183-doc corpus** the gap vanishes (0.706 vs 0.703 ≈ tie) and both match
> their published numbers. SciFact alone does **not** justify Qwen3; the code
> evidence does.

## Code retrieval — the decisive axis

### Published MTEB code tasks (nDCG@10)

| task | nomic | Qwen3-0.6B | Qwen3-4B | Qwen3-8B |
|---|---|---|---|---|
| CodeSearchNet | 0.856 | 0.943 | 0.960 | 0.966 |
| CodeSearchNet-CC | 0.489 | 0.933 | 0.967 | 0.971 |
| CosQA | 0.261 | 0.365 | 0.380 | 0.380 |
| CodeFeedback-ST | 0.543 | 0.864 | 0.895 | 0.899 |
| CodeFeedback-MT | 0.282 | 0.908 | 0.932 | 0.937 |
| CodeTransOcean-Contest | 0.368 | 0.861 | 0.910 | 0.937 |
| SyntheticText2SQL | 0.481 | 0.767 | 0.782 | 0.788 |
| StackOverflowQA | 0.636 | 0.900 | 0.943 | 0.948 |

nomic trails Qwen3 by **+9 to +63**. The **0.6B→4B** gains are real (e.g.
CodeSearchNet-CC +3.4, CodeTransOcean +4.9); **4B→8B is ≤+0.5** everywhere.

### On aimee's OWN code (the real proxy)

1864 functions from `src/**/*.c`: a leading block comment is the query, the
function signature+body is the relevant doc (1:1) — i.e. NL-intent → code-body
retrieval, exactly the `intent_vec`→`body_vec` match aimee performs. Docs capped
to 2000 chars (one clean sequence per doc). Artifacts in
[`benchmarks/results/embedder-gate/aimee-code/`](../../benchmarks/results/embedder-gate/aimee-code/).

| model | nDCG@10 | Recall@10 | embed time | dim | VRAM |
|---|---|---|---|---|---|
| nomic-v1.5 | 0.570 | 0.717 | 20s (1×) | 768 | 0.2 GB |
| **Qwen3-0.6B** (f16) | **0.697** | 0.817 | 217s (11×) | 1024 | 1.2 GB |
| Qwen3-4B (Q8) | 0.7592 | 0.874 | 343s | 2560 | 6 GB |
| **Qwen3-4B** (f16) | **0.7592** | 0.878 | 333s (17×) | 2560 | 8 GB |
| Qwen3-8B (f16) | 0.7608 | 0.880 | 475s (24×) | 4096 | 15 GB |

- **Qwen3-0.6B beats nomic by +12.7 nDCG / +10 Recall@10.**
- **0.6B→4B = +6.2 nDCG (real); 4B→8B = +0.16 (noise)**, confirmed f16-vs-f16.
- **4B-Q8 nDCG == 4B-f16 nDCG (0.7592)** → Q8 is lossless here; 4B-Q8 (4.3 GB) is a
  free option for the GPU default.

## The ladder this produces

| tier | model | dim | when |
|---|---|---|---|
| CPU default | Qwen3-Embedding-0.6B | 1024 | auto (no GPU) — matches pplx-embed's 1024-d, no schema change |
| GPU default | Qwen3-Embedding-4B | 2560 | auto (any GPU) — 8B-grade quality, indexed natively (<4000-d `halfvec` ceiling) |
| GPU opt-in | Qwen3-Embedding-8B | 4000 (trunc 4096→4000) | operator must explicitly configure — buys ~0.2 nDCG for 4096-d native + 2× VRAM |

8B is **not** auto-selected by VRAM: 4B already captures its quality at 1.6× smaller
vectors and ~1.4× faster embed. See
[unified-llm-container](../proposals/pending/unified-llm-container.md) for the
truncation machinery (only the 8B opt-in needs it).

## Serving / harness gotchas

- **Serving (production, HTTP):** `llama-server --embeddings -np 1 --cache-ram 0
  --no-cache-idle-slots` — the default prompt cache fragments the embedding KV cache
  (`GGML_ASSERT(task)` crash); keep `-ub` ≤ 2048 (RADV per-buffer limit). The
  `/v1/embeddings` endpoint returns one vector per input by construction.
- **Code embedding via the CLI harness needs `--no-escape`.** `llama-embedding`
  expands literal `\n`/`\t` (ubiquitous in C string literals) into real newlines,
  splitting one code prompt into many embeddings (1864 docs → 2889). `--no-escape`
  fixes it. **CLI-only** — the HTTP server is unaffected (JSON inputs are discrete).
- **Token density:** C code is ~1.8 chars/token; cap code docs ~2000 chars so each
  stays one sequence (else llama-embedding chunk-splits >ctx prompts).
- **The Qwen3 query instruction contains a newline** (`Instruct: …\nQuery: `);
  the line-based CLI needs it collapsed to a space (negligible quality effect).
- **Sizing:** `ctx 2048` is correct for these corpora; `ctx 8192` made the 0.6B run
  ~20× slower (KV thrash, GPU 15%) for no benefit.

## Caveats

Embedder-isolated retrieval (BEIR text + aimee's own code), **not** the full aimee
pipeline (rerank + fusion). The aimee-code task uses comment↔function pairs as
relevance (constructed, not human-judged), so absolute numbers are indicative; the
**gaps** are the signal and they agree with published MTEB. The formal ship-floor
gate (≥95% of the pplx baseline through `aimee eval`) remains the cutover
precondition.
