# Proposal: aimee-kb LLM endpoint config + zero-config default CPU container

**Status:** pending
**Owner:** deploy/kb

## Principle

**aimee-kb runs no LLM runtime — ever.** It is a thin DB2/curator service that
*calls* an LLM container over HTTP for embedding, reranking, and synthesis. The
inference always lives in a separate `aimee-llm` container (the unified Vulkan
llama.cpp image, CPU or GPU tier) or an external OpenAI-compatible endpoint. The
kb never bundles, bakes, or spawns-in-process any model runtime.

## Desired UX

- **No LLM configured → a CPU `aimee-llm` container is brought up beside the kb
  and the kb points itself at it.** Embedding + reranking + Tier-A synthesis work
  with zero operator config.
- **A GPU or external LLM configured → the CPU container is NOT brought up;** the
  kb uses the configured endpoint (GPU adds higher-dim embed/rerank + Tier-B
  synthesis).
- The operator only ever opts *up* (GPU container or external LLM). CPU is
  automatic.

## Config surface

| Knob | Drives | Default |
| --- | --- | --- |
| `AIMEE_LLM_URL` | embed **+** rerank **+** synth (curator `provider` + `tier_b`, `+/v1`) | the auto-managed CPU sibling |
| `AIMEE_EMBEDDER_URL` | `/embed`, `/embed_batch` | `AIMEE_LLM_URL` |
| `AIMEE_RERANKER_URL` | `/rerank` (split from the embedder) | `AIMEE_EMBEDDER_URL` → `AIMEE_LLM_URL` |

**Precedence (per service):** explicit per-service URL > `AIMEE_LLM_URL` >
auto-CPU default. One `AIMEE_LLM_URL` moves everything to a chosen container;
per-service vars pin one independently.

**Synthesis tiers (Tier-A vs Tier-B).** `AIMEE_LLM_URL` is the "capable container"
knob: it wires **both** Tier-A and Tier-B synthesis to that container's `/v1`. The
zero-config **CPU default does not set `AIMEE_LLM_URL`** — the deploy wires its
embedder + `LLM_ENDPOINT` (Tier-A only), so Tier-B stays idle on the small CPU
model (the weak-model-poisons-the-graph guard, kept). Result: **CPU → embed +
rerank + Tier-A; GPU/external `AIMEE_LLM_URL` → + Tier-B.** No `/health` tier
probe needed.

**Dimension is explicit (default 1024 / CPU).** The embedding dim is operator-set:
**default 1024** (CPU / Qwen3-0.6B, the existing `db2` default); set
`AIMEE_EMBEDDING_DIM=2560` when pointing at the GPU / 4B container. No auto-detect
— the operator who picks the GPU sets the dim and accepts the re-embed.

**Re-embed guard:** CPU = 1024-dim, GPU = 2560-dim — different `model_id` + dim,
so switching a populated kb between tiers is a drop-and-rebuild re-embed (existing
`kb_meta` drift guard); fail closed, never embed under a mismatched dim.

## Phases

- **P1 — config surface (kb code, runtime-independent).** Central resolution of
  embedder / reranker / synth endpoints with the precedence above; `AIMEE_LLM_URL`
  + `AIMEE_RERANKER_URL` consumed by the C in-process embed path, `embed-remote.py`,
  `rerank-remote.py`, and `kb_curator_provider` (Tier-A + Tier-B). Remove the dead
  `embedder:8080` / `llm:8080` baked `ENV` defaults from the **kb image**
  (`Dockerfile`) — on a split deploy they point at non-existent services.
  (`Dockerfile.combined` keeps them: the combined topology ships those services.)
  Unit tests for precedence. *No packaging change.*
- **P2 — zero-config default CPU sibling (packaging).** The `aimee-kb` plugin /
  compose brings up an `aimee-llm-cpu` sibling **by default** and points the kb at
  it; when the operator sets `AIMEE_LLM_URL` to a GPU/external endpoint the CPU
  sibling is not started. The kb does not touch the container runtime — the deploy
  unit owns the sibling's lifecycle.

## Notes
- `.254` stays on the GPU (`AIMEE_LLM_URL=http://10.100.0.1:8742`, 2560-dim) —
  already deployed; this is the durable, general mechanism.
- Supersedes the manual `aimee.yaml` curator wiring done live on `.254`, and the
  earlier (rejected) idea of baking a runtime into the kb image.
