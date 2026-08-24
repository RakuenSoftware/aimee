# Proposal: aimee-kb LLM endpoint config + zero-config default CPU container

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

**Status:** done — P1 (config surface) shipped in #694; P2 (zero-config CPU LLM) delivered
across all deploy topologies via the unified `aimee-llm` architecture, including the
**combined image now bundling + auto-starting the aimee-llm CPU variant** (see the
2026-06-28 reconciliation). P2's original kb-managed-sibling mechanism was **superseded** by
`aimee-llm` as a first-class deploy unit.
**Completed:** 2026-06-28
**Moved from:** `docs/proposals/pending/kb-llm-endpoints-and-default-cpu.md`
**Owner:** deploy/kb

> **Reconciliation (2026-06-25).** **P1 — config surface — is complete and shipped in
> #694** (`bee9fa7`): `AIMEE_LLM_URL` drives embed/rerank/synth (curator Tier-A + Tier-B
> at `{url}/v1`); `AIMEE_EMBEDDER_URL`/`AIMEE_RERANKER_URL` per-service overrides with the
> documented precedence; consumed by the C in-process embed path
> (`config_embedding_command`), `embed-remote.py`, `rerank-remote.py`, and
> `kb_curator_provider` (both tiers); the dead `embedder:8080`/`llm:8080` baked `ENV`
> defaults are removed from the kb `Dockerfile` (`Dockerfile.combined` keeps them);
> embedding dim stays explicit (default 1024). Precedence + fallback are unit-tested
> (`src/tests/test_kb_curator_provider.c`). **P2 — zero-config default CPU sibling — is the
> only residual**, and it is **deploy-tier**: a SmoothNAS plugin / compose change
> (`deploy/smoothnas/aimee-kb.plugin.yaml`) that conditionally brings up an `aimee-llm-cpu`
> sibling and points the kb at it when `AIMEE_LLM_URL` is unset — orchestration that needs
> the SmoothNAS runtime + Docker + the CPU image to validate, so it is deferred to an
> environment that can exercise it (same class as the deployment/CI residual in
> [autonomous-dev-execution-substrate.md](autonomous-dev-execution-substrate.md) §3). The
> proposal stays in `pending/` until P2 lands.

> **Reconciliation (2026-06-28) — closing out: P2's goal delivered, its mechanism
> superseded.** The principle (the kb runs no model and calls a separate `aimee-llm`
> container over HTTP for embed/rerank/synth) and the zero-config CPU UX are both **realized
> in the shipped deploy** — but through a *different* mechanism than P2 sketched, so the
> proposal is closed as done/superseded rather than waiting on the original P2 item:
>
> - **`aimee-llm` is now a first-class deploy unit, not a kb-managed sibling.** The unified
>   Vulkan llama.cpp image (CPU + GPU tiers via `AIMEE_LLM_NGL`) replaced the old
>   torch-embedder + standalone-llm split. The kb never spawns or manages it.
> - **Zero-config CPU default — delivered in `deploy/compose/aimee.yaml`.** The base compose
>   brings up an `aimee-llm` service defaulting to the **CPU** image
>   (`${AIMEE_LLM_IMAGE:-ghcr.io/rakuensoftware/aimee-llm:cpu}`) and points the kb at it by
>   default (`AIMEE_LLM_URL: ${AIMEE_LLM_URL:-http://aimee-llm:8742}`, dim 1024). The
>   operator opts *up* to the GPU tier (2560-dim + `/dev/dri`) by layering
>   `deploy/compose/aimee.gpu.yaml` — exactly the "opt up only" UX in this proposal.
> - **SmoothNAS — `aimee-llm` is its own installable plugin** (`deploy/smoothnas/
>   aimee-llm.plugin.yaml`), and `aimee-kb.plugin.yaml` exposes `AIMEE_LLM_URL` (+ the
>   per-service overrides + `AIMEE_EMBEDDING_DIM`) as config knobs pointed at it. The
>   plugin model installs `aimee-llm` as a peer rather than having the kb conditionally
>   start a CPU sibling — the same outcome, owned by the deploy unit, not the kb.
> - **Combined all-in-one image — bundles + auto-starts the aimee-llm CPU variant.**
>   `Dockerfile.combined` (`WITH_LLM=1`, default on) COPYs the llama.cpp/Vulkan runtime +
>   the unified gateway + baked GGUFs from the prebuilt `aimee-llm-cpu` image;
>   `combined-entrypoint.sh` auto-starts the gateway and points the kb embed path + the
>   curator synth at `127.0.0.1:8742` (dim 1024) — so the default combined container is
>   self-contained (only Postgres external). Operator endpoints win (an external
>   `AIMEE_LLM_URL`/`AIMEE_EMBEDDER_URL`/`LLM_ENDPOINT` is never overridden;
>   `AIMEE_BUNDLED_LLM=off` for the lean image). amd64-only (the llama.cpp runtime is the
>   x64 Vulkan binary); the arm64 combined image stays lean + external.
> - **Validated live:** the CPU tier end-to-end on pve `.253` (CT 150) and the GPU split on
>   `.254` (`AIMEE_LLM_URL=http://10.100.0.1:8742`, 2560-dim). The combined-image bundling
>   ships in the same change as this close-out; its runtime is exercised by the `:testing`
>   combined image on deploy (the image build runs in `publish-testing.yml`).
>
> Net: every part of this proposal's goal is in `testing`; the only "residual" was an
> implementation mechanism that the unified-`aimee-llm` architecture made unnecessary. No
> outstanding work — filed to `done/`.

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
