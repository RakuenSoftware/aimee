# aimee-kb LLM backends: embedding, reranking & synthesis

How to point `aimee-kb` at the model backend that does its **embedding**,
**reranking**, and **synthesis**.

## Principle: the kb runs no model

`aimee-kb` is a thin DB2 / curator service. It **never** runs an LLM or embedder
in-process. It *calls* one over HTTP. Inference lives in a separate **`aimee-llm`
container** (the unified Vulkan llama.cpp stack, deployed as the SmoothNAS
`aimee-llm` plugin) or any external OpenAI-compatible endpoint. `aimee-llm` is a
single **model-less** image: the **tier** (`cpu` / `small` / `mid` / `large`) is
chosen at runtime via `AIMEE_LLM_TIER` and the models are downloaded on first
boot. You only ever give the kb a **URL**. The tiers themselves are documented in
[AIMEE_KB_SYNTH_TIERS.md](AIMEE_KB_SYNTH_TIERS.md).

## Tiers: what each backend provides

All served by the one `aimee-llm` image; `AIMEE_LLM_TIER` picks which models it
downloads on first boot. "Model download" is the first-boot fetch into the
`/models` volume (the image itself is small and model-less).

| `AIMEE_LLM_TIER` | Embedding / reranking | Synthesis | Embedding dim | Model download |
| --- | --- | --- | --- | --- |
| **`cpu`** (default) | Qwen3-Emb-0.6B + ettin-68m | **Tier-A** only (gemma-4-E4B, CPU) | **1024** | ~6.5 GB |
| **`small`** | Qwen3-Emb-4B + ettin-400m | **Tier-A + Tier-B** (Gemma 4 12B) | **2560** (set explicitly) | ~11.4 GB |
| **`mid`** | Qwen3-Emb-4B + ettin-400m | **Tier-A + Tier-B** (Gemma 4 26B-A4B, 24 GB card, 2 slots) | **2560** (set explicitly) | ~17.8 GB |
| **`large`** | Qwen3-Emb-4B + ettin-400m | **Tier-A + Tier-B** (same 26B-A4B, 32 GB card, 4 slots × 256 K) | **2560** (set explicitly) | ~17.8 GB |
| **External LLM** | per the endpoint | per the endpoint | per the endpoint | — |

`small`, `mid`, and `large` share the 2560-dim embedder, so a KB moves between them
with no re-embed. `large` is identical to `mid` bar `SYNTH_SLOTS=4` (4 concurrent
agents for a 32 GB card). Pick by synth need + card size, not by the KB — switching
is a `AIMEE_LLM_TIER` change (the new tier downloads to its own `/models` subdir).

*Tier-A* = mechanical extract/index passes. *Tier-B* = reasoning passes (judge,
resolve-entities, contradictions, **synthesize**, promote). A small CPU model is
deliberately **not** allowed to run Tier-B (the weak-model-poisons-the-graph
guard), so CPU-only deployments get retrieval + Tier-A synthesis; Tier-B turns on
when you point the kb at a capable container via `AIMEE_LLM_URL`.

## Zero-config default

If **no** LLM is configured, the kb deployment brings up a **cpu-tier `aimee-llm`
container beside it** (`AIMEE_LLM_TIER=cpu`) and points itself at it: embedding,
reranking, and Tier-A synthesis work with nothing to set. The operator only opts
*up*: set `AIMEE_LLM_TIER=small|mid|large` (with a GPU) or point at an external LLM
and the CPU sibling is not started.

> The default CPU sibling is owned by the **deploy unit** (the smoothnas plugin /
> compose), not the kb process. The kb never touches a container runtime.

## Config surface

All optional. Precedence is **per service**: an explicit per-service URL wins,
then `AIMEE_LLM_URL`, then the zero-config CPU default.

| Variable | Drives | Default |
| --- | --- | --- |
| `AIMEE_LLM_URL` | embedding **+** reranking **+** synthesis (Tier-A + Tier-B, at `{url}/v1`) | the auto CPU sibling |
| `AIMEE_EMBEDDER_URL` | embedding (`/embed`, `/embed_batch`) | `AIMEE_LLM_URL` |
| `AIMEE_RERANKER_URL` | reranking (`/rerank`) | `AIMEE_EMBEDDER_URL` → `AIMEE_LLM_URL` |
| `AIMEE_EMBEDDING_DIM` | pgvector schema width | `1024` (CPU); set `2560` for the GPU tier |
| `AIMEE_LLM_MODEL` | model label sent to `AIMEE_LLM_URL` | `aimee-synth` (gateways ignore it) |
| `LLM_ENDPOINT` | **Tier-A synth only** (small-model interface) | unset |

- **`AIMEE_LLM_URL` is the one knob.** Point it at a container and embedding,
  reranking, and synthesis all use it. `{AIMEE_LLM_URL}` serves `/embed`,
  `/embed_batch`, `/rerank`; `{AIMEE_LLM_URL}/v1` serves chat completions for the
  curator. Give the base URL **without** `/v1` (a trailing `/v1` or `/` is
  tolerated).
- **Split a service out** by setting `AIMEE_EMBEDDER_URL` and/or
  `AIMEE_RERANKER_URL`. They override `AIMEE_LLM_URL` for just that service.
- **Auth:** the kb sends **no bearer** on the embed/rerank/synth path, so the
  container must run **auth-off** (empty `AIMEE_LLM_AUTH_TOKEN`). This is safe on
  the internal deployment network (the `aimee-llm` gateway is not exposed
  off-host).

### Embedding dimension

The dim is **explicit**, not auto-detected:

- **Unset → 1024**: the CPU / Qwen3-0.6B tier.
- **GPU / 4B tier → set `AIMEE_EMBEDDING_DIM=2560`.**

Changing the dim against a **populated** kb is a **drop-and-rebuild re-embed**:
1024 and 2560 are a different model identity *and* width, and the `kb_meta` drift
guard refuses to serve a new embedder against a mismatched corpus. Plan a
re-embed (snapshot → drop dim-sized `halfvec` tables → rebuild at the new dim →
parity gate) when moving between CPU and GPU tiers. See
[runbooks/unified-llm-cutover.md](runbooks/unified-llm-cutover.md).

## Examples

**GPU container (this deployment's `.254`):**
```
AIMEE_LLM_URL=http://10.100.0.1:8742      # embed + rerank + Tier-A&B synth
AIMEE_EMBEDDING_DIM=2560                   # GPU / 4B tier
```

**External embedder, GPU for synth:**
```
AIMEE_LLM_URL=http://10.100.0.1:8742       # synth (+ rerank, unless split)
AIMEE_EMBEDDER_URL=https://embed.internal  # pin embedding elsewhere
AIMEE_EMBEDDING_DIM=<that model's dim>
```

**Default (nothing set):** the deploy brings up the cpu-tier `aimee-llm` sibling
(`AIMEE_LLM_TIER=cpu`); retrieval + Tier-A synthesis work at 1024-dim with no
configuration.

## Notes for plugin / compose operators

The `aimee-kb` smoothnas plugin and the compose topologies expose `AIMEE_LLM_URL`
/ `AIMEE_EMBEDDER_URL` / `AIMEE_RERANKER_URL` / `AIMEE_EMBEDDING_DIM` as config.
The kb image carries **no** baked embedder/LLM endpoint defaults. An unset
backend falls back to the 384-dim builtin embedder (test/shim only), so a real
deployment either relies on the default CPU sibling or sets one of the URLs above.
