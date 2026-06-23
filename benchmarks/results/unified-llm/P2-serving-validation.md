# Unified-llm P2 — `.254` Vulkan serving validation (2026-06-23)

Empirical validation of the unified-llm-container model serving on **`.254`**
(AMD Radeon RX 7900 XTX, RADV/**Vulkan** 1.4.305, 32c/157 GB), llama.cpp build
**b9761**. Confirms the embedder + reranker decisions before the container is built.

## Embedder — Qwen3-Embedding (Vulkan, `--embeddings --pooling last`)

Serving flags (the proposal's required config): `--ctx-size 8192 -ub 512 -np 1
--cache-ram 0 --no-cache-idle-slots -ngl 99`.

| model (GGUF) | output dim | Vulkan | crash w/ flags |
|---|---|---|---|
| `Qwen/Qwen3-Embedding-0.6B-GGUF` (f16) | **1024** | ✅ RADV NAVI31 | none |
| `Qwen/Qwen3-Embedding-4B-GGUF` (Q8_0) | **2560** | ✅ | none |

Dims match the proposal (0.6B=1024, 4B=2560). No `GGML_ASSERT(task)` prompt-cache
fragmentation with `--cache-ram 0 --no-cache-idle-slots`. `-ub 512` honours the RADV
per-buffer limit.

## Reranker — Ettin (the decisive finding)

**Ettin reranks via llama.cpp as ENCODER + a gateway-side Dense head — NOT native
`/v1/rerank`.** `cross-encoder/ettin-reranker-{400m,68m}-v1` are sentence-transformers
models (`config.json` arch `ModernBertModel`, `num_labels:None`); the score head is the ST
pipeline (`modules.json`): Pooling → `2_Dense` (1024→1024, GELU) → `3_LayerNorm` →
`4_Dense` (1024→**1**). `convert_hf_to_gguf.py` (even with
`--sentence-transformers-dense-modules`) does **not** carry this multi-Dense head, so a
naive GGUF is encoder-only and **misranks**. (Community GGUFs like
`keisuke-miyako/ettin-reranker-v1-gguf`, tagged `feature-extraction`, are this headless
encoder — do not use.)

**Working recipe (validated):**
1. Convert the encoder: `convert_hf_to_gguf.py models/ettin-reranker-400m-v1 --outtype f16`.
2. Serve: `llama-server -m ettin-400m.gguf --embeddings --pooling cls -fa on -ngl 99`.
3. Gateway, per `(query, doc)`: `v = /v1/embeddings("query</s>doc")` (1024-d, CLS) →
   `h = GELU(v @ W2ᵀ)` → `h = LayerNorm(h; γ=norm.weight, β=norm.bias)` →
   `score = h @ W4ᵀ + b4`. Head ≈ 4 MB (`2_Dense`/`3_LayerNorm`/`4_Dense` safetensors),
   pure numpy, **no torch**. The LayerNorm (`norm.weight`/`norm.bias`) is essential.

Toy-gate scores (relevant vs irrelevant; higher = more relevant):

| model | SciFact (cardiac claim) | Code (python sort) |
|---|---|---|
| **ettin-400m** (GPU) | **8.93** vs 1.10 ✅ | **9.21** vs −1.58 ✅ |
| **ettin-68m** (CPU)  | **8.26** vs 5.19 ✅ | **10.09** vs 1.07 ✅ |
| bge-reranker-v2-m3 (ref) | 0.50 vs −10.99 ✅ | 5.61 vs −10.93 ✅ |

Both ettin tiers rank correctly with confident separation. `-fa on` is required on this
Vulkan build (the prior LATENCY.md: ettin-400m top-20 0.34s with FA vs 0.76s without).

## Synth

The gemma synth GGUFs (E4B / 12B / 26B-A4B) are already on `.254`
(`/mnt/.../synthbench/models/`) from the curator-synth benchmark (RESULTS.md); not re-run
here.

## Implication for the container

llama.cpp serves all three roles; the **reranker's Dense head runs in the gateway** (P3) —
the only non-llama.cpp compute, ~4 MB of weights, baked into the image alongside the
encoder GGUF. Models are **baked into two images** (`aimee-llm-cpu` / `aimee-llm-gpu`,
operator-directed) rather than runtime-fetched. Recipe + convert env recorded for reuse.
