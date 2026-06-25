# Proposal: self-contained aimee-kb + one-knob embed/rerank/synth endpoints

**Status:** pending
**Owner:** deploy/kb

## Problem

The split deployment (`aimee-server` + `aimee-kb` + external `aimee-llm`) wires
**embedding + reranking** from one var (`AIMEE_EMBEDDER_URL` → the unified
container's `/embed`, `/rerank`) but leaves **synthesis** pointed at a dead baked
default. The kb image still bakes `AIMEE_EMBEDDER_URL=http://embedder:8080` and
`LLM_ENDPOINT=http://llm:8080/v1` from the *old combined* topology, so on a split
deploy the curator/synthesis LLM calls hit a non-existent `llm:8080` and silently
do nothing. Standing up synthesis on `.254` required hand-editing the live
`aimee.yaml` (`kb.curator.provider` + `tier_b`) — exactly the manual wiring this
proposal removes.

Two goals:
1. **Self-contained retrieval.** `aimee-kb` should embed + rerank out of the box
   with no external container (CPU models baked in).
2. **One optional knob to upgrade.** A user deploys the GPU `aimee-llm` and sets a
   single address to move embed + rerank + synth onto it — and may also override
   the embedder and reranker endpoints independently.

## Design

### Default (zero-config)
The `aimee-kb` image **bakes a CPU embedder + reranker** (Qwen3-Embedding-0.6B /
1024-dim + ettin reranker, served locally). Retrieval works with only postgres +
kb; no `aimee-llm`, no torch embedder. Synthesis is **off** until an LLM endpoint
is set (the bundle is embed+rerank only — not synth).

### Endpoint config surface (all optional)
| Knob | Drives | Default |
| --- | --- | --- |
| `AIMEE_EMBEDDER_URL` | `/embed`, `/embed_batch` | baked CPU embedder |
| `AIMEE_RERANKER_URL` | `/rerank` (split from embedder) | `AIMEE_EMBEDDER_URL` → baked CPU reranker |
| `AIMEE_LLM_URL` | embed **+** rerank **+** synth (curator `provider` + `tier_b`, `+/v1`) | unset → bundled CPU embed/rerank, synth off |

**Precedence (per service):** explicit per-service URL > `AIMEE_LLM_URL` > baked
CPU default. So `AIMEE_LLM_URL` alone moves everything to the GPU container;
`AIMEE_EMBEDDER_URL`/`AIMEE_RERANKER_URL` independently pin one service elsewhere.

`AIMEE_LLM_URL` is the single "point me at the unified `aimee-llm` container"
knob: it sets the embedder/reranker base (`/embed`,`/rerank`) **and** the synth
base (`+/v1`, curator Tier-A `provider` and Tier-B `tier_b`), each only when not
explicitly overridden. Tier-B gets a fallback here (it normally has none — the
"weak model poisons the graph" guard) because `AIMEE_LLM_URL` is the operator
explicitly designating one *capable* container for synth.

### Dimension / re-embed guard
CPU tier = **1024-dim** (0.6B), GPU tier = **2560-dim** (4B): different `model_id`
**and** dim. Switching a populated kb between bundled-CPU and GPU is a
drop-and-rebuild **re-embed** (the `kb_meta` drift guard already enforces this).
The knob must fail closed with a clear "re-embed required" message — never embed
new vectors under a mismatched dim/identity.

## Phases

- **P1 — config surface (no image change).** Central resolvers for embedder /
  reranker / synth endpoints with the precedence above; `AIMEE_RERANKER_URL` +
  `AIMEE_LLM_URL` consumed by the C in-process paths, `embed-remote.py`,
  `rerank-remote.py`, and `kb_curator_provider` (Tier-A + Tier-B). **Remove the
  dead `embedder:8080` / `llm:8080` baked `ENV` defaults** from `Dockerfile` /
  `Dockerfile.combined` (compose files still set them explicitly for their
  topologies). Unit tests for precedence + the re-embed guard. Plugin manifest
  gains `AIMEE_LLM_URL` / `AIMEE_EMBEDDER_URL` / `AIMEE_RERANKER_URL` config keys.
- **P2 — bake CPU embed/rerank into the kb image.** Add the llama.cpp embed +
  rerank runtime + the 0.6B / ettin GGUFs + a local supervisor to the `aimee-kb`
  image; kb defaults to the localhost embed/rerank when no override is set. Image
  size + cold-start budget recorded. CI CPU smoke.

## Non-goals / notes
- `.254` stays on the **GPU** (`AIMEE_LLM_URL=http://10.100.0.1:8742`, 2560-dim) —
  already deployed; this proposal is the durable, general mechanism.
- Synth stays external (no synth GGUF baked into kb) per the embed+rerank-only
  bundle decision; the GPU `aimee-llm` (or any OpenAI-compatible endpoint) is the
  synth provider.
- Relates to `docs/runbooks/unified-llm-cutover.md` and the `aimee-llm` plugin.
