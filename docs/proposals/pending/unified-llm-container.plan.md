# Implementation plan: one unified `aimee-llm` container

Companion to [unified-llm-container.md](./unified-llm-container.md) (rev 7, roundtable
**signed off at rev 6**). Base branch: `testing`. Each packet is **independently shippable
as its own PR** — built/lint/unit-tested + roundtable-reviewed + (where applicable)
empirically validated on `.254` before the next, per the autonomous-proposal loop.

## Grounding against the tree (2026-06-23)

Reconciled before planning (the lesson from the exec-substrate run — don't build what
exists):

- **The shared LLM-provider client exists** — `src/provider_client.c` +
  `src/headers/provider_client.h` (curator-llm-backend §1, already used by
  `kb_curator_extract.c` / `kb_curator_llm.c` / `memory_rewrite_llm.c`). The proposal
  reuses it; it is **not** new work. What's new is shipping it as the gateway's wire layer.
- **The "llm" container is the *stock* `ghcr.io/ggml-org/llama.cpp:server` image** (no
  custom Dockerfile), serving Gemma-4-E4B via `LLAMA_ARG_HF_REPO`, behind the
  `curator-llm` compose profile. So a llama.cpp synth path already runs.
- **The embedder is the torch container** — `Dockerfile.embedder` (python:3.11-slim +
  CPU torch + `sentence-transformers`) running `scripts/embedder-server.py`, serving
  `pplx-embed-v1-0.6b` (1024) / `-4b` (2560) + `ettin-reranker`. This is what the fold
  deletes. `embed-remote.py` / `rerank-remote.py` and `AIMEE_EMBEDDER_URL` are untouched.
- `EMBED_MAX_DIM` is **2560** today (`src/headers/aimee.h:95`).
- Benchmark harness exists: `benchmarks/results/synth/{serve_and_bench.sh,synth_bench.py}`,
  `docs/validation/embedder-gate-scifact.md` — reused for the `.254` empirical gates.

## Verification environment

This dev host has **no Docker, no GPU, no models**. The verification path is three-tier:

1. **In-env (this host).** Pure C (`make build/unit-tests/lint`) and the gateway's pure
   Python logic (router / handlers / wire-adapters / health / cap+413 / typed errors) via
   `unittest` with mocked child-supervisor + upstreams. No model needed.
2. **`.254` empirical (AMD 7900 XTX, 32c/157 GB; access: `admin@192.168.1.254`).**
   **llama.cpp uses the Vulkan backend (Mesa RADV) — not ROCm** (operator-directed): build
   `-DGGML_VULKAN=ON`, GPU via `/dev/dri/renderD128`; `/dev/kfd` (ROCm) is **not** used.
   Vulkan is also **vendor-agnostic** (one build serves AMD/Nvidia/Intel), which collapses
   §5's per-vendor build matrix to **CPU + Vulkan** (CUDA/ROCm become optional perf opt-ins,
   not the default path). Run `llama-server` **directly** (outside any container) with the
   pinned GGUFs + serving flags to validate the model decisions before packaging:
   `/v1/embeddings` dims, `/v1/rerank` correctness + `-fa on`, the
   `-np 1 --cache-ram 0 --no-cache-idle-slots -ub 512` requirement (the RADV per-buffer
   limit is why `-ub ≤ 2048`), latency vs the proposal's table, and the ship-floor /
   tier-cutoff gates against the real corpus. Reuses `serve_and_bench.sh`. **This is the
   primary GPU validation host.**
3. **CI + tierd deploy.** The OCI image builds in `.github/workflows/publish-images.yml`
   (**CPU + Vulkan** variants — a Vulkan image needs only Mesa/ICD + libvulkan, no
   ROCm/CUDA toolchain or `/dev/kfd`, so it builds on stock GHA runners) with the
   proposal's CPU smoke tests (bind-validator, canary-log, memory-scrape, kill-a-child,
   load-sequence, contract). `.254` runs containers via **tierd → LXC** (not raw docker;
   tierd API `127.0.0.1:8420`), with **GPU passthrough = `/dev/dri/renderD128` + the Vulkan
   ICD** (no `/dev/kfd`); deploy = publish image → tierd plugin manifest-swap (the recipe in
   the .254 deploy memory). The **production cutover** (corpus re-embed) is an
   operator-gated maintenance op on `.254`.

Every packet states which tier verifies it. Nothing GPU- or cutover-tier is auto-claimed.

## Packet sequence (smallest / safest / most-foundational first)

| # | Packet | Verifies on |
|---|--------|-------------|
| **P1** | **Dim foundation** — `EMBED_MAX_DIM 2560→4000`; `(model_id, dim)` embedder + `(model_id, scoring-contract)` reranker drift guard; compat admission rule | **SHIPPED #644** |
| **P2** | **`.254` Vulkan serving validation** — Qwen3-Emb-0.6B/4B (→1024/2560, flags OK) + Ettin-400m/68m rerank validated; the **reranker recipe = encoder GGUF + gateway Dense head** (native `/v1/rerank` can't carry the ST head); results in `benchmarks/results/unified-llm/P2-serving-validation.md` | **DONE (.254, b9761)** |
| **P3** | **Gateway rework** — decompose `embedder-server.py` → router / handlers / wire-adapters (reuse `provider_client`) / child-supervisor / observability; `/embed`,`/embed_batch`(512→413),**`/rerank` = embed(CLS) + the ettin Dense head (numpy)**, `/v1/chat/completions`(`stream`→typed 400), four-state health | **in-env** (logic unit) + **.254** |
| **P4** | **Two BAKED images `aimee-llm-cpu` / `aimee-llm-gpu`** (Vulkan llama.cpp + s6 + gateway + **tier GGUFs + the ettin head weights baked in**); delete `Dockerfile.embedder` + the `embedder`/`llm` services | **CI** (build + CPU smoke) + **.254** (tierd LXC) |
| **P5** | **`install.sh` GPU detection + model registry/sizing** — detect a GPU + VRAM (sysfs/`rocm-smi`/`nvidia-smi`/`lspci`) → pick the **single Vulkan build** for any vendor (CPU is the explicit final branch); VRAM→registry-row, startup VRAM validation + `gated` state, sizing table + load-order CI test | in-env (logic) + **.254** (AMD/Vulkan branch) |
| **P6** | **§1a security hardening** — mTLS default + bind-`0.0.0.0` startup assertion, derived `X-Aimee-Scope` RBAC, per-user cred pass-through (`mlock`+memmove zero, no-swap/no-core), circuit breakers, audit row + escape-hatch lint | in-env (unit) + CI (canary/memory-scrape) |
| **P7** | **Migration / ship-floor gate / cutover** — controlled drop-and-rebuild re-embed, `maintenance`-gate + parity, ship-floor ≥95% (rerank+fusion vs pplx fixture) **pre-cutover in staging on .254**, deploy-tag-revert net | **.254** (live corpus) — operator-gated |

P1–P3 detailed below; P4–P7 get their own plan revision once P2's empirical numbers + P3's
gateway shape are fixed (they may re-scope the registry rows / sizing). **Order rationale:**
the dim guard (P1) and the empirical serving numbers (P2) are the load-bearing facts every
later packet depends on, and both are reachable now (in-env + `.254`) without the container.

**P1-before-P2 is safe (roundtable plan-gate):** the dims P1 locks (`0.6B=1024`,
`4B=2560`, `8B=4000`) are **architecturally fixed by the named models**, not pending P2 —
P2 validates *serving correctness + quality* (the right vectors come out, rerank ranks
right, latency), not the dimensions. So P1's drift-guard tuple is content-pinned against
facts that can't move. P1 ships the *machinery* (record/compare `(model_id, dim)`) keyed on
the live embedder's own identity; it does not assert a 4000-d model exists — the live torch
embedder keeps emitting its current dim and P1 records *that* identity, so nothing strands.

---

## P1 — dim foundation + model-identity drift guard (THIS PR after the plan gate)

Pure C/SQL, fully in-env-verifiable; unblocks every dim- and model-identity-dependent
surface so later packets don't churn the schema.

- **`src/headers/aimee.h`:** `EMBED_MAX_DIM 2560 → 4000` (exact), with the comment updated
  to state the `halfvec` index ceiling (4000 inclusive) and that 4096-native is
  unindexable. `EMBED_MAX_OUTPUT` follows. Audit every fixed `[EMBED_MAX_DIM]` buffer +
  any `2560` literal that meant "max dim" (grep gate in the test).
- **Drift guard → model identity, not just dim.** Extend the existing
  `db2_embedding_dim_record_or_check` / `kb_meta schema_embedding_dim` (PR #337) to record
  and compare **`(embedder model_id = repo@sha, dim)`** and **`(reranker model_id,
  scoring-contract = /v1/rerank|fa=on)`**. A mismatch refuses startup / rejects writes (a
  same-dim different-model swap — e.g. `pplx-embed` 1024 ↔ `Qwen3-0.6B` 1024 — is the exact
  footgun this closes). Re-embed/re-rank triggers key on **model_id change**.
- **`schema_embedding_dim_compat` admission rule:** a compat-listed model is admitted only
  if **cosine ≥ 0.99 on a fixed aimee probe set vs the prior model**, *or* it is a
  documented same-repo retraining (new sha). A compat entry that fails the probe is
  **refused** (the list can't re-open the same-dim/different-space hole).
- **Migration shape (defined here, executed P7):** snapshot → drop dim-sized `halfvec` →
  recreate at new `(model_id, dim)` → replay → `maintenance`-gate until parity; in-place
  `halfvec` type change forbidden. **Paired with a checked-in, idempotent schema-revert
  migration** (recreate the prior dim-sized `halfvec` from the snapshot), CI-round-tripped
  before cutover, so a mid-cutover failure has a single rollback recipe (schema-revert +
  the P7 deploy-tag-revert), not just a deploy-tag flip.
- **Tests (in-env):** dim-buffer sizing at 4000; record-then-match passes; **same-dim
  different-model_id refuses**; reranker scoring-contract mismatch forces re-rank; compat
  admit/refuse by the 0.99 probe; the `4096→4000 slice + L2-renorm` proxy invariant
  (assert output ≤4000, fail loud) — unit-tested with a synthetic 4096 vector.
- **Not in P1:** the 8B tier is opt-in and its truncation path is exercised by the test
  but no 8B model is pulled; the GPU default (4B/2560) needs no truncation.

## P2 — `.254` empirical serving validation (no code; a checked-in fixture + results)

De-risks every model decision before the gateway/container is built. On `.254`, via
`serve_and_bench.sh` (extended), with the proposal's pinned GGUFs:

- **Embed:** `Qwen3-Embedding-0.6B-GGUF` (CPU, 1024) + `-4B-GGUF` (GPU, 2560) under
  `--ctx-size 8192 -ub 512 -np 1 --cache-ram 0 --no-cache-idle-slots`; confirm
  `/v1/embeddings` returns the stated dim, one vector/input, no `GGML_ASSERT(task)` crash.
- **Rerank:** `ettin-reranker-400m` (GPU) / `-68m` (CPU) over native `/v1/rerank` with
  **`-fa on`**; confirm correctness (an irrelevant doc does **not** top the rank — the
  Qwen3-Reranker failure) + the ~2× FA throughput + the latency table (top-20 ≈0.34 s GPU).
- **Output:** `tests/fixtures/pplx_baseline.json` (pplx/ettin GGUF SHA256s, corpus-snapshot
  id, query set, expected metrics, serving config) pinned by content hash — the ship-floor
  fixture P7 gates against. Plus a short results note under `benchmarks/results/`.
- **Fixture is load-bearing → reviewed like code.** P2 ships as an explicit **fixture PR**
  (the codebase's checked-in-baseline pattern, e.g. `pplx_baseline.json`): roundtable-gated,
  metrics pinned by content hash, corpus-snapshot id recorded, with a **maintenance
  contract** — a named owner, the P7 tolerance bands it's gated against (ship-floor ≥95%
  nDCG/MRR + rank-agreement ≥0.95), and the update procedure (a same-dim retrain under a new
  sha re-runs the `.254` validation and re-pins the fixture in a reviewed PR; never edited
  by hand).
- **Verify on `.254`; nothing here is auto-claimed in CI** — it's the empirical record the
  later gates reference.

## P3 — gateway rework (decomposed, logic unit-tested in-env)

`scripts/embedder-server.py` → a package: `router` (per-role mode dispatch) · `handlers`
(`embed`/`embed_batch`/`rerank`/`synth`) · `wire_adapters` (llama.cpp `/v1/*` ↔ aimee
`/embed`+`/rerank`; openai/anthropic via `provider_client` semantics) · `child_supervisor`
(shim over s6; mockable) · `observability` (corr-id, per-role/mode metrics, audit row).

- **Rerank handler = encoder embed + the ettin Dense head (P2 finding).** The `/rerank`
  handler does NOT proxy a native `/v1/rerank` (the ST head doesn't convert). For each
  `(query, doc)`: call the local ettin **encoder** llama-server `/v1/embeddings`
  (`query</s>doc`, CLS pooling, batched over the candidate set), then apply the baked-in
  head `score = (GELU(v @ W2ᵀ) → LayerNorm(γ,β)) @ W4ᵀ + b4` in numpy (~4 MB weights loaded
  once at startup from the image). Returns the aimee `/rerank` shape. Unit-tested with a
  recorded encoder-output fixture (no model) asserting the head reproduces the P2 toy-gate
  scores; `.254`-validated against the live ettin encoder.

- **Pure-logic unit tests (in-env, no model):** mode table (`local-cpu|local-gpu|forward|
  external`) per role; `/embed_batch` **512-cap → 413**; `stream=true` → typed
  **`400 {"error":{"code":"streaming_unsupported"}}`**; four-state health
  (`ready|loading|gated|down`) incl. a dead child → role `down`; kb readiness hits
  `/health/embed` (+`/health/rerank`) not blocked by synth loading; wire-adapter
  round-trips with a fake upstream.
- **`.254` integration (local modes):** point the gateway at the P2 Vulkan llama-servers;
  assert `/embed` + `/rerank` parity with direct calls and the `embed_batch` profile. This
  exercises **`local-cpu`/`local-gpu`** empirically; **`forward`/`external` stay logic-only
  in P3** (unit-tested against a fake upstream) — labelled `first-empirical-validation-at-P4`
  (when a second in-cluster endpoint exists to point `forward` at), never silently claimed.
- Health/typed-error **contract test** asserts shapes match the pinned
  curator-llm-backend version.

## Cross-cutting

- **Build/test discipline:** `make -j1` on the dev host (LTO flakes); `make lint` +
  `make unit-tests` before every push; never merge red (`gh pr checks`). No
  `Co-Authored-By`. Branch off `origin/testing`, squash, human-only promotion to `main`.
- **Per-packet loop:** plan-gate (roundtable) → implement → in-env green → `.254`/CI
  validate where applicable → roundtable review → PR → green CI → squash-merge → next.
- **Plan gate:** this plan is roundtable-reviewed before P1 code (the proposal itself is
  signed off; this gates the *decomposition + verification strategy*, especially the
  `.254`-vs-CI-vs-cutover split).
