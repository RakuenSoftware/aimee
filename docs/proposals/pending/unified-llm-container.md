# Proposal: One unified `aimee-llm` container for all LLM use (local CPU/GPU · forward · external)

- **State:** draft — **rev 7** · **roundtable SIGNED OFF at rev 6** (round 5: 0
  blocking / 0 high / 0 medium, converged). Arc: r1 10 blocking → r2 7 blocking +
  8 high → r3 1 blocking + 1 high + 2 medium → r5 **0 blocking**. rev 7 closes the
  two open questions (pins the operator's synth models; decouples + shrinks the
  reranker to 0.6B-default, 8B dropped; E4B-ungated CPU default). See "Decisions
  taken" and "Changelog".
- **Author:** JBailes
- **Date:** 2026-06-21
- **Base:** `testing`. Today there are **two** model containers: a torch /
  sentence-transformers **embedder** (`Dockerfile.embedder` +
  `scripts/embedder-server.py`, `pplx-embed` + `ettin-reranker` over a custom
  `/embed` + `/rerank` contract) and a separate **`llm`** container
  (`llama.cpp:server` running **Gemma E4B** over OpenAI `/v1`).
- **Companions:**
  - [curator-llm-backend](curator-llm-backend.md) **(pin: rev as of 2026-06-14
    READY)** — its **shared LLM-provider client** (§1), **stage→provider routing**
    (§2), **health four-state contract** (§4), and **drain policy** (§4/§5) are
    **reused as the foundation**. This proposal **amends its §3 packaging**: the
    shared client ships as a **library**, and the curator runs as an **isolated
    s6-supervised process inside `aimee-llm`** (§6). A gateway **contract test**
    asserts the imported four-state + typed-error shapes match that pinned version.
  - [embedder-runtime-fetch-autodim](embedder-runtime-fetch-autodim.md) — its
    runtime-fetch-once posture, the **auto-dim handshake**, and the
    `schema_embedding_dim` **drift guard** (PR #337) are reused and **extended to a
    `(model_id, dim)` guard for both embedder and reranker** (§2).

## Goal

**Delete the standalone embedder container and the standalone E4B `llm`
container.** Replace both with **one** `aimee-llm` container that is the single
gateway for **every** LLM use in aimee — embeddings, reranking, and synthesis. For
each use, the container is backed by one of **four modes**, chosen by config:

1. **local-CPU** — run the model in-container on CPU (a `llama-server` GGUF).
2. **local-GPU** — run it in-container on GPU (`--n-gpu-layers`, by detected VRAM).
3. **forward** — proxy to *another local* OpenAI-compatible server
   (llama.cpp / vllm / Ollama / …).
4. **external** — proxy to an external API (**Codex/OpenAI** = `openai` wire;
   **Claude** = `anthropic` wire).

aimee-kb and aimee-server always talk to **one endpoint** (the `aimee-llm`
container); the container decides per-role whether to serve locally or forward.

## Why one container

- **One access layer for credentials, routing, auth, budget, and observability.**
- **Kills the torch dependency and the two-container split.**
- **The fold does not block off-box synth.** Running synth on a different GPU host
  is the `forward`/`external` mode — no second container needed. The unified
  container is the *control plane*; heavy synth can live elsewhere. Blast-radius
  tradeoff handled in §6, not waved away.

## Design

### 1. The `aimee-llm` container = gateway + supervised local runtime

One image: the **llama.cpp runtime** + a **role-based gateway** (reworked
`scripts/embedder-server.py`) under **s6-overlay**. Endpoints:

- **Retrieval (unchanged for the kb):** `POST /embed`, `POST /embed_batch`,
  `POST /rerank` — `embed-remote.py`, `rerank-remote.py`, kb config defaults, and
  `AIMEE_EMBEDDER_URL` are **untouched**.
- **Synthesis:** OpenAI-compatible `POST /v1/chat/completions` (grammar/JSON-schema
  via `--jinja`). **Streaming is disabled in the first release**: `stream=true`
  returns a **typed `400 {"error":{"code":"streaming_unsupported"}}`** clients can
  branch on (documented so SDKs that default to streaming set `stream=false`),
  rather than an opaque failure. Enabling streaming later must update the audit row
  + mid-stream circuit-breaker handling in the same change.
- **Health:** per-role `GET /health/{embed,rerank,synth}` + aggregate `/health`
  (§4).

**Per-role mode** (`embed` / `rerank` / `synth`):

| Mode | Gateway does | Local proc? | Upstream auth |
|------|-------------|-------------|---------------|
| `local-cpu` | supervise a `llama-server` GGUF (CPU) | yes | n/a |
| `local-gpu` | supervise a `llama-server` GGUF (`--n-gpu-layers`) | yes | n/a |
| `forward` | proxy/translate to another OpenAI base_url | no | **operator-supplied upstream key in gateway config; gateway authenticates outbound; upstream must be on the internal network or its own auth boundary** |
| `external` | proxy/translate to Codex (`openai`) / Claude (`anthropic`) | no | per §1a (operator secret or hardened per-user pass-through) |

A **local** role runs its own inner `llama-server` (one model+mode per process). A
**forward/external** role runs no local process — the gateway translates wire
formats via curator-llm-backend's shared client. **`/embed_batch`** reuses the
embed handler with a **concrete per-call cap of 512 vectors/call** and a stated
p95 latency budget (a distinct bulk-backfill load profile in the sizing table,
§5), not a separate mode; an oversize batch returns **`413`**. CI tests both the
cap and the `413` path.

**Process isolation (pays down the SPOF the fold creates).** s6-overlay supervises
each local role's `llama-server` as an independent service. Two *separate*
mechanisms (do not conflate):

- **Restart isolation — from s6:** restart-on-failure *per role*; an embed crash
  restarts embed and does **not** take down rerank or synth. The router maps a
  dead child to that role's `down` health state.
- **Memory isolation — from compose, not s6:** cgroup v2 caps come from per-child
  `mem_reservation`/`mem_limit` in compose (§5), so one role cannot starve the
  others.

A **kill-a-child smoke test** (CI on every gateway PR **and** a post-deploy hook;
failure **blocks the deploy**) kills one inner `llama-server` and asserts the
others keep serving, the dead role reports `down` then recovers within budget
(inner restart ≤10 s; `loading→ready` ≤30 s). If a deployment cannot provide
cgroup isolation, synth falls back to a separate sidecar container sharing the
image (config-only; §6).

**Gateway module decomposition:** `router` · per-role `handlers` · `wire-adapters`
· `child-supervisor` shim · `observability` — each independently testable.

### 1a. Security & access control (the gateway is privileged)

- **Mutual auth, mTLS by default.** kb/server ↔ `aimee-llm` uses **mTLS** in the
  shipped compose (self-signed certs are fine for single-host); a shared **bearer
  token is dev-only opt-in** (bearer-with-TLS-off is a downgrade surface).
  Unauthenticated requests are rejected.
- **Network pin, asserted.** A startup **bind-address validator exits non-zero
  with a typed error if the resolved bind is `0.0.0.0`**, run **before** any
  privileged op (key load, supervisor bring-up); covered by a CI test.
- **Rate + budget.** Rate-limit **key = mTLS peer-cert subject (preferred) or
  bearer-token *hash*, never the raw bearer**; per-IP only as a coarse outer
  limit. A per-scope **daily token/spend budget** caps `external` spend.
- **Scope is derived, never trusted from the wire.** Every request's
  **`X-Aimee-Scope` (`curator|user`) is set by the gateway from the authenticated
  identity** (mTLS cert CN or bearer identity); a caller-supplied `X-Aimee-Scope`
  header is **rejected** (fail-closed). Router RBAC keys off the derived scope so
  an operator key can never attach to a `user` request or vice-versa.
- **Per-user credential pass-through, hardened** (carries §3). When aimee-server
  forwards a user's session credential:
  1. the carrying hop is **mTLS**, never plaintext;
  2. **`Authorization` redacted** in access *and* error logs; a **canary test**
     asserts no log line for a synthetic request contains the token;
  3. container disables **core dumps** and runs **no swap**;
  4. the credential page is **`mlock`-ed** and **zeroed via `ctypes.memmove` over
     a `bytearray`** after completion (CPython strings are immutable, and no-swap
     alone is insufficient — a page can be read back from swap; hence mlock). A CI
     **memory-scrape test** injects a canary token, then scans post-completion RSS
     and asserts the canary is absent from every mapped page;
  5. an `Authorization`-derived value is **never** a cache key;
  6. user session tokens are **short-lived** (≤5-min TTL);
  7. **Claude:** recommended posture is operator-issued **admin keys + per-user
     attribution via Workspaces/metadata**; raw end-user-key passthrough is an
     opt-in, documented as conflicting with Anthropic's admin-key model.
- **Escape hatch, narrowed.** aimee-server → provider directly bypasses gateway
  controls, so it is an **operator flag, off by default, requiring documented
  justification** (the justification lives as a deploy-time annotation in the
  config repo / runbook entry; a CI lint **fails if the flag is enabled with an
  empty justification**). Even on the direct path, aimee-server emits a **thin
  audit row** — scope, provider, outcome, request-id, byte counts, **and a
  `user_id` (or salted `session_id_hash`)** so per-user attribution and the §4
  per-scope spend budget still work on the path that most needs them — **never**
  creds. A CI assertion requires any non-`curator`-scope direct-path row to carry a
  non-empty user identifier; retention follows the §4 audit-row lifecycle.

### 2. Model registry — two independent ladders, sized by GPU capability

Embed/rerank and synth scale on **separate** ladders, selected independently at
install by detected VRAM (§5).

**Embed ladder** (the embed row's dim drives every dim-dependent surface):

| Install | Embed (GGUF) | dim |
|---------|--------------|-----|
| **all tiers** | **`nomic-ai/nomic-embed-text-v1.5-GGUF`** | **768** |

> **Embed ladder collapsed to a single model — nomic-embed-text-v1.5 (768-dim),
> 2026-06-22** (provisional; see "provisional" note below). The whole multi-tier
> Qwen3 embed ladder is **withdrawn** on direct empirical evidence —
> [embedder-gate-locomo](../../validation/embedder-gate-locomo.md). On the LoCoMo
> direct-retrieval screen (7900XTX/Vulkan, 1982 questions), nomic **beats every
> Qwen3-Embedding size** on Recall@5/MRR — including the **8B at 4096-dim** — at
> the **smallest** vector size (768). Qwen3-4B gave **no** lift over 0.6B; Qwen3-8B
> costs **5.3× the vector dim** (storage / HNSW build / scan) for a *worse* R@5 and
> only an R@10 tie. Roundtable (architect + engineer) was unanimous: **nomic
> everywhere; drop the 4B and 8B tiers** — an optional high tier is config surface,
> a second GGUF slot, and operator confusion for no measurable win. A single
> embedder also removes the dim-dependent tier-selection logic and the
> 8B-MRL-truncation machinery entirely (nomic is fixed 768-d, not truncatable).
>
> **Provisional — retrieval-only, ship-floor gate pending.** This screen is
> *embedder-isolated* (no reranker, no fusion, English, one benchmark, 1982 q) and
> does not yet report tokens/s or VRAM. It is sufficient to set the **proposal**
> default but is **not** the production cutover: the full-pipeline ship-floor gate
> (rerank + fusion, nDCG/MRR vs the pplx baseline) is the ship precondition and can
> still reorder this. If a future multilingual / long-context need appears, revisit
> the Qwen3 tiers then.

**Reranker — decoupled, not part of the embed ladder.** The reranker scores
`(query, candidate)` text pairs, so it is **dimension-agnostic** and need not scale
with the embedder. Per the Qwen3 paper (Table 4), the **4B matches or beats the
8B** (MTEB-R 69.76 vs 69.02; FollowIR 14.84 vs 8.05), so **8B is dropped
entirely**, and the 0.6B is within ~3–4 pts of 4B on general retrieval (the gap is
larger, ~8 pts, only on *code* retrieval).

| Role | Default | Optional upgrade |
|------|---------|------------------|
| Reranker (all tiers) | `Qwen/Qwen3-Reranker-0.6B-GGUF` | `Qwen/Qwen3-Reranker-4B-GGUF` — an **embedder-independent** knob, recommended only for **code-heavy** deployments. **No 8B.** |

**Synth ladder** (pinned to your specified models; MoE rows have low active params):

| Install | Synth model (GGUF) |
|---------|--------------------|
| CPU-only (**default**) | **`ggml-org/gemma-4-E4B-it-GGUF`** (Gemma 4 E4B — the mirror is **ungated**, no `HF_TOKEN`) |
| GPU-S | `unsloth/gemma-4-12B-it-GGUF` (Gemma 4 12B) |
| GPU-M | `unsloth/gemma-4-26B-A4B-it-GGUF` (Gemma 4 26B-A4B, MoE ~4B active) |
| GPU-L | `unsloth/Qwen3.6-35B-A3B-GGUF` (Qwen3.6 35B-A3B, MoE ~3B active) |

> **Pinning.** Names are fixed above; implementation pins each to a specific
> **commit sha** + quant. The CPU default is **Gemma 4 E4B via the ungated
> `ggml-org` GGUF mirror** (already pulled unauthenticated in the current compose),
> so there is **no separate "ungated fallback" model and no first-boot `HF_TOKEN`
> requirement** — E4B is simply the CPU default.

#### The 8B truncation — why 4000, and how it must be done

> **SUPERSEDED (2026-06-22):** the embed ladder collapsed to a single 768-dim
> nomic model (above), so there is no 8B embedder and this truncation machinery is
> not built. Retained only as design rationale should the Qwen3 tiers ever be
> revisited (multilingual / long-context).

Qwen3-Embedding-8B is **MRL-trained** (loss on first 512/1024/2048 + full 4096),
so information front-loads into early dims; published guidance shows truncating to
**1024 retains ~95%**. Trimming only **4096→4000 (last 96 dims, ~2.3%)** costs far
less — expected **<1% nDCG/MRR**. The "safer" alternatives are worse: 2048 is a
50% cut; capping at 4B/2560 forfeits the 8B→4B gap. **4000 is the most recall the
index allows.**

- **4000 = pgvector `halfvec` *index* ceiling (inclusive).** `halfvec`
  HNSW/ivfflat indexes up to **4,000** dims; native 4096 would be **unindexable**.
- **Storage ≠ index.** A `halfvec` column can *store* >4000 but only ≤4000 is
  indexable; 4000 sits exactly at the ceiling (zero headroom, fine for a fixed dim).
- **Truncation-order invariant:** `llama.cpp emits 4096 → proxy slices first-4000
  + L2-renormalize → kb stores 4000 into halfvec(4000)`. `EMBED_MAX_DIM=4000`
  (exact) cannot hold 4096, so the slice is in the **proxy**, which **asserts its
  output ≤ 4000 and fails loudly** otherwise.
- **Tier-cutoff gate (≥99%):** enabling the 8B tier requires cosine Pearson r
  (full-4096 vs 4000) **≥ 0.99** and nDCG@10 retention **≥ 99%** vs full-4096.
- **Metric note:** the post-slice **L2-renormalize changes vector norms**, so it is
  distance-preserving for **cosine** (the kb's canonical similarity end-to-end) but
  **not** for raw dot-product / L2-distance. Any consumer using those must account
  for the renormalization; documented here and in `docs/retrieval-stack.md`.

#### Model-drift guard — embedder `(model_id, dim)`, reranker `(model_id, scoring-contract)`

A dim-only guard is insufficient — two different models can share a dim (e.g.
`pplx-embed` and `Qwen3-Embedding-0.6B` are both 1024-d), so it would silently mix
incompatible spaces. (The new default, `nomic-embed-text-v1.5`, is 768-d, so a
swap *from* pplx's 1024-d **does** also change the dim, but the guard must not rely
on that.) The guard records and compares **embedder model identity
(repo@sha) + dim** and refuses startup / rejects writes on mismatch, keying the
re-embed trigger on **model_id change**. **The reranker gets the same guard** (keyed
on reranker `model_id` + scoring contract): a reranker swap invalidates cached
scores, so a mismatch forces a re-rank pass rather than serving stale scores
alongside new-embedder results.

A `schema_embedding_dim_compat` allow-list permits deliberate upgrades **only**
under a codified admission rule, asserted at load: admit a model iff **cosine ≥
0.99 on a fixed aimee probe set vs the prior model**, *or* it is a documented
parameter-identical retraining (same base repo, new sha). A compat-listed model
that fails the probe is **refused** — so the list cannot re-open the same-dim /
different-space footgun it exists to manage.

**Migration is a controlled drop-and-rebuild, never an in-place rewrite:**
snapshot → drop dim-sized `halfvec` tables → recreate at new `(model_id, dim)` →
replay source rows through the new embedder (bounded batch) → **gate the kb to
`maintenance` until corpus parity** (pre-drop vs post-backfill counts; divergence
→ `degraded`). In-place `halfvec` type changes against a live kb are forbidden.

#### Reranker contract + embedding acceptance gates (named, distinct)

The Qwen3-Reranker mapping is **pinned, not deferred**: fixed chat template, fixed
yes/no target tokens, `temperature=0`, documented logprob→score transform, and a
query-side instruction prefix (query path only; pooling documented). Two
**distinct** quality gates (do not conflate):

- **Ship-floor gate (≥95%)** — gates *replacing* pplx/ettin, and it is a
  **pre-cutover precondition** evaluated in staging/CI against the real corpus
  **before** the production migration runs (the pplx/ettin baseline numbers are
  recorded as fixtures once, so nothing needs pplx/ettin running in production):
  nDCG@10 / MRR@10 ≥ 95% of baseline on a representative set, **plus**
  rank-agreement (top-k agreement ≥ 0.95 *or* Spearman ≥ 0.9 — distribution-only
  checks can pass while ranking regresses). **Gate failure → freeze and
  investigate; do not cut over.** Because the gate is upstream of the cutover,
  **pplx/ettin is removed completely** with no in-release retention (see "What this
  removes").
  - **Reproducibility:** the baseline is a **checked-in fixture**
    (`tests/fixtures/pplx_baseline.json`) pinned by content hash — pplx/ettin GGUF
    SHA256s, corpus-snapshot id, the pinned query set + expected metric values, and
    the staging config (llama.cpp version, quant, hardware class) that produced
    them; CI asserts the gate evaluates against the pinned hash.
  - **Staging↔production parity** (what makes the gate predictive): matching
    hardware class, llama.cpp version pin, the corpus snapshot id at cutover, and
    the query-set composition — documented as the gate's validity precondition.
  - **Freeze semantics:** a named operator owns declare/unfreeze; unfreeze requires
    a root cause **and** a fix re-validated against the same corpus+queries; the
    staging env is held for investigation. The freeze blocks only the cutover, not
    ongoing pplx/ettin production serving (which still exists until cutover).
- **Tier-cutoff gate (≥99%)** — to *enable the 8B truncated tier* (above).

### 3. Synthesis on both credential scopes

- **kb-level curator synth** — operator secret in gateway config; runs in the
  isolated **curator process** (§6).
- **per-user `aimee-server` synth** (`reflect`, `synthesize`, MoA) — client-held /
  session-cached, governed by the §1a pass-through controls + escape hatch.

### 4. Health, degradation & observability

- **Per-role readiness.** `/health/{embed,rerank,synth}` each report the four
  states (`ready`/`loading`/`gated`/`down`). The **kb readiness check hits
  `/health/embed`** (+`/health/rerank` if reranking) so it indexes as soon as
  embed is ready, without waiting for synth to load. Aggregate `/health` (with a
  `detected_accelerator` field) stays for external monitors. A **contract test**
  asserts these states/typed-errors match the pinned curator-llm-backend version.
- **Circuit breakers (a control, not a label).** Per-**provider, per-mode** on
  `forward`/`external`: **5 consecutive errors OR >50% error rate over 30 s →
  open**; **60 s recovery-probe interval**; **1 success in half-open → closed**.
  On open: per-role fallback (e.g. external `down` → `local-cpu` synth if the tier
  allows) else a **typed `503 provider_unavailable`** the kb/server retry policy
  consumes. State machine unit-tested.
- **Observability.** Correlation ID propagated from kb/server; per-role per-mode
  metrics (count, p50/p95 latency, tokens in/out, error rate); an **audit row per
  `/v1/chat/completions`**: **metadata-only by default (no prompt/response
  content)**, `scope`/mode/provider/outcome, **encrypted at rest, operator-only
  ACL, 30-day hot retention then aggregate-only**.

### 5. GPU detection & sizing

- **Install-time detection, multi-vendor.** `install.sh` probes **NVIDIA
  (`nvidia-smi`)**, **AMD ROCm (`rocm-smi`/sysfs)**, and **Intel (`lspci -d 8086:`
  / oneAPI)**; picks the matching llama.cpp build; maps VRAM → registry row. **CPU
  is the explicit final branch**, not a silent fallthrough; unsupported
  accelerators are documented. (**Apple Metal** is a *native-dev-only* manual
  override, not an `install.sh` branch — the Linux/Docker deployment target never
  exercises it, so it stays out of the tested path.)
- **Startup validation + swap transition.** At start (before launching `llama-server`)
  the gateway validates **available VRAM vs the model's need**; if insufficient it
  **falls back to the CPU-tier model or reports `gated`**, never silently
  `--n-gpu-layers` on a too-big model. During the swap window the affected
  `/health/<role>` returns **`loading`** and in-flight requests get a typed
  **`503 provider_unavailable`**.
- **Sizing table (published per tier), gateway included.** Peak RSS per inner
  `llama-server` at its quant **+ gateway/s6 overhead (reserve ~512 MB) + headroom**,
  reserved in the **container-level `mem_limit`** so three children at their caps
  cannot OOM the parent. **Startup load order embed → rerank → synth**, each with a
  per-step memory budget, plus a CI test that the load sequence stays within
  `mem_limit`. Includes a **batch (`/embed_batch`) load profile**. On a
  RAM-constrained CPU box, **CPU-synth is opt-in** (default synth → forward/external).
- **CPU synth = Gemma 4 E4B, ungated, no token.** The CPU default is the
  **`ggml-org/gemma-4-E4B-it-GGUF`** mirror, already pulled **unauthenticated** in
  the current compose — so a fresh CPU-only install is turnkey with **no `HF_TOKEN`
  and no license step** (§2). `HF_TOKEN`-as-a-Docker-secret handling is retained
  **only** for operators who opt into a *gated* model (e.g. an official Google repo
  or another gated GGUF): supplied as a **Docker secret / mounted file, not an env
  var**, **deleted after download + checksum**; an absent/invalid token for a
  selected gated model → clear **`gated`** health state, never a crash-loop.

### 6. Process isolation & the curator-fold tradeoff (explicit)

We take the fold but pay down its cost:

- **One image, isolated processes.** The shared client is a **library**; curator
  runs as its **own s6-supervised process** (own restart policy; memory cap from
  its compose child limits, per §1 wording) — *not* the same process as user-synth.
- **Cred-path separation by RBAC** — operator-cred (curator) and user-cred
  (per-user synth) run in **different processes**, separated by the derived
  `X-Aimee-Scope` (§1a).
- **Off-box scaling preserved** via `forward`/`external` — no un-folding.
- **Residual, stated honestly.** A single *container* is one restart/upgrade unit,
  and RBAC is **logical, not OS-level** — a *kernel-level* exploit in curator has
  the same blast radius as one in user-synth. The **sidecar fallback** (synth as a
  separate container sharing the image, config-only) is the **documented response
  to a suspected kernel-level compromise**.

## Files touched (map, not exhaustive)

- **Delete:** `Dockerfile.embedder`; the `embedder` + `llm` services in
  `compose.yaml` / `compose.server.yaml` / `compose.combined.yaml` /
  `deploy/container/aimee*.yaml` collapse into one `aimee-llm` service (per-child
  mem limits + GPU reservation + mTLS certs).
- **New/rework:** `Dockerfile.aimee-llm` (llama.cpp + s6-overlay + gateway);
  `scripts/embedder-server.py` → decomposed gateway. `embed-remote.py` /
  `rerank-remote.py` **unchanged**.
- `.github/workflows/publish-images.yml` — one `aimee-llm` image (CPU / CUDA /
  ROCm variants) + the new CI tests (bind-validator, canary-log, memory-scrape,
  kill-a-child, load-sequence, contract).
- `install.sh` — multi-vendor GPU detection + per-role mode/model/dim env.
- `src/headers/aimee.h` — `EMBED_MAX_DIM` 2560 → **4000 (exact)**.
- Drift guard → `(model_id, dim)` for embedder **and** reranker + compat-list +
  admission rule; shared-client library extraction (curator-llm-backend §1).
- `docs/retrieval-stack.md` — Qwen3 tables; unified container; 4000 truncation +
  index-vs-storage note.

## What this removes

- The torch embedder container/image and the `pplx`/`ettin` pairing — **removed
  completely in the cutover release.** No retention, no built-but-non-default
  image, no in-image rollback flag. Rollback does **not** depend on keeping the old
  embedder around: the ship-floor gate is a **pre-cutover precondition** (§2,
  validated in staging against the real corpus *before* the production migration),
  so a regression never ships; and a post-cutover revert is a **deploy-level
  rollback to the prior release tag** (which still contains pplx/ettin) plus the
  reverse re-embed — an explicit maintenance op. This resolves the
  rollback-vs-removal contradiction by moving the gate upstream of the cutover, not
  by retaining pplx/ettin.
- The standalone `llm` container (Gemma 4 E4B is **retained as the default CPU
  synth model**, now served inside `aimee-llm` via the ungated GGUF mirror).
- curator-llm-backend's separate `aimee-curator` container (§3 packaging),
  superseded by the isolated curator *process* (§6).
- The hand-synced `embedding_dim` footgun **and** the same-dim model-swap footgun
  (both caught by the `(model_id, dim)` guard).

## Migration

- Adopting the unified container re-embeds the corpus **once** (pplx→Qwen3 is a
  `model_id` change), via the controlled drop-and-rebuild (§2) with parity gating.
  **pplx/ettin is removed completely**; the ship-floor gate (§2) is validated
  **pre-cutover** in staging against the real corpus, so a regression never ships.
  A post-cutover revert, if ever needed, is a **deploy-level rollback to the prior
  release tag** (which still contains pplx/ettin) plus the reverse re-embed — an
  explicit, gated maintenance operation, not an instant in-image toggle.
- **The deploy-tag-revert is a one-time safety net** for the *initial* cutover
  only: once the first post-cutover release is validated, no prior tag still
  contains pplx/ettin, and rollback is no longer deploy-tag-based. The prior
  release tag is therefore **retained in the registry for a defined post-cutover
  window** (e.g. 6 months) per image-retention policy.
- **Re-embed cost (plan the window):** document expected wall-clock per million
  vectors at the chosen embed model, peak RSS per inner `llama-server`, and whether
  the backfill runs alongside read traffic or needs a maintenance window (the kb is
  gated to `maintenance` during parity backfill, §2).
- CPU synth defaults to Gemma 4 E4B via the ungated GGUF mirror (no `HF_TOKEN`).

## Decisions taken (through rev 7)

- **pplx/ettin is removed completely** in the cutover release — no retention, no
  in-image rollback flag. The ship-floor gate is a **pre-cutover staging
  precondition** (freeze-investigate on failure); any post-cutover revert is a
  deploy-level rollback to the prior release tag + reverse re-embed — a **one-time**
  safety net for the cutover release, with the prior tag retained in the registry
  for a defined window.

- **Embed = single model `nomic-embed-text-v1.5` (768-dim)**, provisional on LoCoMo
  evidence (beats every Qwen3 size incl. 8B at 4096-dim; see embedder-gate-locomo)
  pending the full-pipeline ship-floor gate. The multi-tier Qwen3 embed ladder
  (0.6B/4B/8B) is **withdrawn** (roundtable-unanimous); no embed tier-selection and
  no 8B-MRL-truncation machinery — nomic is fixed 768-d.
- **Curator fold: take it** as an isolated supervised process + RBAC; off-box
  synth via forward/external; sidecar fallback = kernel-compromise response.
- **Per-user synth via the gateway** with §1a hardening; **escape hatch** is an
  off-by-default operator flag that still audits.
- **Drift guard → `(model_id, dim)` for embedder and reranker** + admission-gated
  compat-list.
- **Auth default = mTLS**; bearer dev-only.
- **CPU synth default = Gemma 4 E4B** via the ungated `ggml-org` GGUF mirror (no
  `HF_TOKEN`); the separate "ungated fallback" model is dropped.
- **Synth models pinned** to the operator's spec: Gemma 4 **E4B (CPU)**, Gemma 4
  12B / 26B-A4B / Qwen3.6 35B-A3B (GPU-S/M/L). **Reranker decoupled and shrunk:
  0.6B default everywhere, 4B optional for code-heavy, 8B dropped** (4B ≥ 8B per
  Qwen3 Table 4); the reranker guard is keyed on `(model_id, scoring-contract)`,
  not dim.
- **Streaming disabled** in the first release.
- **Two named gates:** ship-floor ≥95% (replace pplx/ettin) vs tier-cutoff ≥99%
  (enable 8B truncated tier).

## Open questions

1. **Commit-sha + quant pins** — model *names* are fixed (§2); implementation pins
   each to a specific commit sha and selects the quant per tier (Q4_K_M vs Q5/Q6).
   Mechanical, not a design decision.
2. **Reranker re-rank pass cost** on a reranker-only swap — confirm recomputing
   cached top-K rerank scores is cheap (it should be: the reranker writes no corpus
   vectors, so a swap needs no re-embed/re-index, only a score refresh).

## Risks

- **Embedding-quality regression** pplx/ettin → Qwen3 — gated by the named §2
  acceptance gates, validated **pre-cutover** in staging (freeze-investigate on
  failure); post-cutover revert is the one-time deploy-tag rollback (§Migration).
- **Gateway as privileged single point** — mitigated by §1a (auth/budget/scope),
  §4 (circuit breakers + per-role health + audit), §6 (process isolation);
  residual: one restart/upgrade unit + logical-not-kernel RBAC → sidecar fallback.
- **Per-user credential exposure** — mitigated by §1a controls + canary +
  memory-scrape tests; residual handled by the audited direct-to-provider escape.
- **`halfvec` at exactly 4000** — zero index headroom; enforced by the proxy
  `≤4000` assertion.
- **Turnkey "no `HF_TOKEN`" depends on the ungated `ggml-org` E4B mirror staying
  available + ungated.** If the mirror is removed or later gated, the CPU default
  breaks; mitigate by pinning the mirror by commit sha, caching the GGUF in the
  model volume, and documenting the gated-official-repo `HF_TOKEN` path (§5) as the
  fallback.

## Changelog

- **rev 7 (2026-06-21):** reduced the open questions to mechanical items
  (commit-sha/quant pins + a reranker-swap-cost confirmation). **Pinned synth
  models** to the
  operator's spec — Gemma 4 E4B (CPU, **ungated `ggml-org` mirror → no `HF_TOKEN`**,
  so the rev-5 "ungated fallback" model is dropped and E4B is the plain CPU
  default), Gemma 4 12B / 26B-A4B / Qwen3.6 35B-A3B (GPU-S/M/L). **Decoupled the
  reranker** from the embed ladder (it is dimension-agnostic): **0.6B default on all
  tiers, 4B optional for code-heavy, 8B dropped** (per Qwen3 Table 4 the 4B ≥ 8B).
  Embed GGUF repos named. Updated §2 ladders, §5 (retitled "GPU detection &
  sizing"), "What this removes", Migration, Decisions.
- **rev 6 (2026-06-21):** roundtable **signed off** (round 5: 0 blocking/high/medium,
  converged). Folded the round-5 polish: ship-floor baseline pinned as a checked-in
  hashed fixture; staging↔production parity precondition; freeze semantics
  (owner/unfreeze criteria); deploy-tag-revert flagged as a **one-time** cutover
  safety net + image-retention window; re-embed cost/maintenance-window note.
- **rev 5 (2026-06-21):** operator directive — **pplx/ettin removed completely**
  (no one-release retention). Resolved the round-3 rollback-vs-removal
  contradiction by moving the **ship-floor gate upstream of the cutover** (staging
  precondition, freeze-investigate on failure) instead of retaining the old
  embedder; post-cutover revert is now a deploy-level rollback to the prior release
  tag + reverse re-embed. Updated "What this removes", §2 ship-floor gate,
  Migration, and Decisions.
- **rev 4 (2026-06-21):** addressed round-3 (1 blocking + 1 high + 2 medium + 4
  suggestions): **deferred torch/pplx/ettin deletion one release** so the
  ship-floor **rollback is executable** (resolves the rollback-vs-removal
  contradiction); **`user_id`/`session_id_hash` on the escape-hatch audit row** +
  CI assertion; renamed §5 "Runtime re-probe" → **"Startup validation + swap
  transition"** to match the body; **concrete `/embed_batch` cap (512) + `413`** +
  CI; typed **`streaming_unsupported` 400**; **Apple Metal demoted** to
  native-dev-only override; escape-hatch **justification storage + CI lint**;
  **L2-renorm/cosine-canonical metric note**.
- **rev 3 (2026-06-21):** addressed round-2 (7 blocking + 8 high): compat-list
  **admission rule**; **ungated CPU-synth default** + pinned-fallback requirement;
  **bind-`0.0.0.0` startup assertion**; **circuit-breaker parameters** + state
  machine; **escape-hatch narrowed** + audited; **forward-mode upstream auth**;
  **scope-tag derived (anti-spoof)**; **s6-vs-compose isolation wording**;
  **reranker drift guard** + score caches; **audit-row lifecycle**; **GPU re-probe
  transition**; **gateway overhead + load order** in sizing; **named ship-floor vs
  tier-cutoff gates**; **mlock+memmove credential zeroing** + memory-scrape test;
  rate-limit key, **mTLS default**, **streaming disabled**, rank-agreement metric,
  contract-test pin, smoke-test ownership, Intel detection, batch profile, kernel
  residual.
- **rev 2:** added §1a auth + pass-through hardening, §4 per-role health + circuit
  breakers + audit, §5 GPU re-probe + sizing + E4B gating, §6 curator fold as
  isolated process; extended drift guard to `(model_id, dim)`; pinned reranker
  contract + acceptance gates; expanded 8B-truncation rationale.
- **rev 1:** initial unified-container draft.
