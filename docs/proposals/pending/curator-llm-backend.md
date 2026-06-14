# Proposal: Pluggable curator LLM backend (shared provider client + optional curator container)

- **State:** reviewed — READY (roundtable 2026-06-14: security · architect · QA · contrarian; 4 rounds to convergence)
- **Author:** JBailes
- **Date:** 2026-06-12
- **Base:** `origin/main` (v0.2.50). The retrieval pipeline is live and default-on:
  indexer → 0.6B embedder → reranker (ettin-400m) → curator + KB-docs, with code/
  memory/doc embeddings populating automatically during ingest (shipped v0.2.46–
  v0.2.50). The deep-curator stages are default-on but **no-op without a configured
  LLM** — this proposal supplies that LLM as a pluggable, decoupled backend.
- **Scope:** a new shared LLM-provider client (extracted from `aimee-server`),
  a kb-level operator-owned provider layer + stage→provider config, the curator
  drain (`src/kb/kb_curator_drain.c`) routing stages through the shared client and
  dropping the rate limiter, a new optional `aimee-curator` container
  (`Dockerfile.curator` + publish-images matrix), and a curator block on the kb
  `/v1/health` surface. Config: `kb_curator_provider` + per-stage overrides;
  retire/repurpose the `kb_curator_*_command` sidecar hooks. No new capability flag.

## Goal

Make the curator LLM a **pluggable, decoupled, operator-owned backend**. An
operator can run the curator on a **small model fetched at runtime** (the default
is **Gemma 4 E4B**, which takes a one-time HF license acceptance + `HF_TOKEN`; an
ungated Apache-2.0 model such as **Granite 4.1 3B** is the zero-touch
alternative), a **large local model** (e.g. on their own GPU), or **any external
API** — selected by config, with no coupling to the embedder and no dependence on the
per-user `aimee-server` instances.

## Background — the constraint that shapes everything

The topology is **N `aimee-server` : 1 `aimee-kb`**. The kb is the *shared*
knowledge service; the servers are per-user, and their provider credentials are
**client-held / session-cached** (never on disk, never necessarily in the kb).
The curator runs in the shared kb and curates the *shared* knowledge graph.

Consequences:

- The kb **cannot** borrow a particular user's server/model/credentials/budget to
  curate shared knowledge — which one? whose cost? So the curator LLM is a
  **kb-level / operator concern**, not a per-user one.
- The curator credential is therefore a **single operator/deployment secret**
  (same class as the DB2 password or `AIMEE_EMBEDDER_URL`), legitimately held in
  kb config/env — a *different* class from the per-user client-held keys.

## Design

### 1. Shared LLM-provider client (DRY)

`aimee-server` and `aimee-kb` have the identical problem: *given a provider def
(`base_url` + `wire_api` + `model` + key) and a request (messages, optional
JSON-schema/grammar), return a completion, handling wire-formats and retries.*
That machinery lives in the server today. **Extract it into a shared module** both
link:

| Consumer | Provider defs from | Credential scope |
|---|---|---|
| `aimee-server` (per-user) | user config | client-held / session-cached |
| `aimee-kb` (operator) | kb/operator config | operator deployment secret |

Same calling code; only the source of provider defs + credentials differs. This
is the foundation — §2/§4 are built on it.

### 2. Curator stages → providers (two-tier default)

The curator is a pipeline of stages with very different capability/cost profiles:

- **Tier A — extract / index** (`extract_docs`, `extract_code`, `index_narrative`,
  `index_claims`, `index_code_unit`, `link_artifacts`): read a chunk, emit
  entities/claims/links as **grammar-constrained JSON**. High volume (one call per
  chunk/unit), mechanical. A **small model is sufficient** — the grammar carries
  format reliability.
- **Tier B — reason / judge** (`judge`, `resolve_entities`,
  `detect_contradictions`, `synthesize`, `promote_entity`): reason *over* the
  extracted content. Low volume, but this is where a weak model **poisons the
  graph**. Needs a **capable** model.

Each stage maps to a **named provider**; the default is two-tier:

- **Tier A → runtime-fetched small model** (default **Gemma 4 E4B**; default-on; see §3).
- **Tier B → gated BYO-strong** — *no weak fallback*: the reasoning stages stay
  idle until the operator configures a capable provider.

Config: a default `kb_curator_provider` with per-stage/per-tier overrides
(`kb_curator_extract_provider`, …). The existing per-stage hooks
(`kb_curator_extract_command` / `judge_command` / `synthesize_command`) are
**repurposed from sidecar scripts to provider references**.

### 3. `aimee-curator` container (optional, swappable)

A new **optional** container — the llama.cpp runtime plus a thin entrypoint, with
**no model baked in**. The model is **fetched at runtime** into a persistent cache
volume on first boot and reused thereafter, the way you'd run any llama.cpp /
Ollama server:

- **llama.cpp server** exposing an **OpenAI-compatible** `/v1/chat/completions`
  with **JSON-schema / grammar** support (`--jinja`) — so it is *just a provider*
  the kb points at.
- **`CURATOR_MODEL=<hf repo / gguf>@<commit-sha>`** as an **env var** (not a build arg),
  **pinned to a revision** by default (an upstream re-tag can't silently swap the
  model). The entrypoint pulls it once into the **curator's own** model volume —
  its **own** mount, *not* the embedder's `/opt/models`, so the two containers
  never race to write the same cache on a cold start; later starts reuse it.
  Pinning edge cases, resolved at boot so a real first boot never lands in a
  silent "loading forever" trap: a `CURATOR_MODEL` with **no `@<sha>`** (a moving
  tag) is **rejected at boot** with a "pin a revision" message; a revision that's
  been deleted/GC'd on HF maps to `gated`/`not-found` distinctly from a network
  `down`; and a **multi-part GGUF** (`*-00001-of-0000N.gguf`) is pulled via a
  directory pointer (all parts) — the default ships a single-file model.
- **No license-bundling gate, but a consent path.** We ship a *runtime*, not
  weights, so the published image redistributes nothing — the operator fetches
  the model (same posture as `ollama pull`), and any license applies to *them*.
  **Gemma 4 E4B is the default Tier-A model.** But Gemma is **gated on
  HuggingFace** (license acceptance + token), so a bare first boot 401/403s until
  `HF_TOKEN` (with the license accepted) is set — the entrypoint must surface
  that as a clear "this model is gated; set HF_TOKEN or pick an ungated model",
  never a silent hang. For a turnkey ungated default, an Apache-2.0 model (e.g.
  Granite 4.1 3B) is the alternative; E4B stays the recommended pick when the
  operator has accepted its terms. `CURATOR_MODEL` runs operator-chosen weights —
  treat it as trusted-by-the-operator (optional org/repo allowlist).
- **Small image.** No multi-GB model layers — just the llama.cpp runtime. Weights
  live in the cache volume, never in the published image (no slow release builds,
  no 8 GB pulls).
- **CPU by default; GPU-optional** build variant (ROCm/CUDA).

Because the model is an env var and the service is "just a provider", **one image
serves any tier**: a small instance (CPU) → Tier A (default Gemma 4 E4B); a large
instance (`CURATOR_MODEL` → 27–32B, on a GPU) → Tier B; or skip it entirely and
point Tier B at an external API. It rides **next to** the embedder, never inside it.

**Same pattern as the embedder** (see `embedder-runtime-fetch-autodim`): a thin
runtime image, model fetched **once** into a persistent cache volume, reused on
every restart/recreate. The curator only differs in default and flexibility —
Tier-A default (Gemma 4 E4B) on a small CPU instance, or `CURATOR_MODEL` → a
27–32B (GPU) / external API for Tier B. The download is one-time and a
provisioning-only network dependency; an air-gapped deploy pre-populates the
cache volume. (Contrast the *old* embedder, which baked the model at build time
for a no-runtime-network start; the cache-volume approach gives the same
restart-time guarantee without the multi-GB image.)

### 4. Curator health / observability

Extend the kb `/v1/health` (already reports `db2_ok`/`embed_ok`/…) with a
**curator block**, plus a `aimee kb curator status`-style readout:

- **Per provider, a four-state contract** (not a boolean): `ready` (model loaded,
  serving) · `loading` (model still downloading/loading — the curator container's
  own `/health` returns 503 `{status:"loading"}`) · `gated` (fetch 401/403 — model
  needs `HF_TOKEN`/license; **distinct** from down because the operator action
  differs: accept-license vs investigate-network) · `down` (unreachable / erroring).
  A bare "reachable?" TCP probe is not enough — a gated curator answers TCP but
  401s every call.
- **Pipeline progress:** queue depth (`kb_code_unit_jobs` pending/done), curator
  vectors produced, last-curation-at, per-stage throughput, plus per-tier
  `pending` and `dead_letter` counts (see the drain policy).

This removes the **silent no-op** — including making "Tier B: no provider
configured → reasoning stages idle" visible, which is the real harm from the
curator being inherently operator-provisioned (we can't make it fully turnkey, but
we can make its state legible).

**Stage-unavailable drain policy (no silent poisoning, no unbounded retry).** A
stage that errors or has no provider must **never** advance a chunk past it with
a degraded/empty result — it parks the chunk at its last-completed stage (earlier
output kept; the chunk stays retrievable, just less enriched) and retries with
capped exponential backoff. This applies to **both tiers**:

- **Tier B** (`judge`/`synthesize`/…): park in `pending_tier_b`; `/v1/health
  .curator.tier_b = down` when no provider is reachable.
- **Tier A** (`extract`/`index`/…) is upstream of every chunk, so a Tier-A outage
  (e.g. the bundled E4B fetch failed) halts the pipeline — it gets the **same**
  contract: `pending_tier_a`, `/v1/health.curator.tier_a` in the four-state set
  above. Don't leave it implicit.
- **Bounded retries + dead-letter.** Each stage has a max-attempt cap
  (`kb_curator_tier_{a,b}_max_attempts`, default 5). On exceed, the chunk's stage
  moves to a `kb_curator_dead_letter` table with its last error and stops
  retrying — so a permanently-misconfigured provider (wrong model, revoked key,
  schema always rejected) can't grow the queue without bound. Surface
  `dead_letter_count > 0` as a **distinct** health field from `down`: "provider
  up but consistently rejecting" is the easiest-to-miss, most-important state.
  Dead-lettered chunks are visible + replayable via `aimee kb curator status` /
  a requeue command.

A reasoning/extraction stage that errors is a retry (then dead-letter), never a
"good enough" promotion — the difference between "not curated yet" and "curated
wrong".

### 5. Remove the rate limiter

Drop `kb_curator_max_jobs_per_hour` and the sliding-window ring buffer in the
drain. The curator drains the backlog continuously, then idles. Natural throttle =
backend throughput; cost control for a paid external backend lives at the
**provider** (endpoint choice / its own limits), not an artificial jobs/hour cap.
This also simplifies the drain loop.

## Alternatives considered

- **Curator → aimee-server → provider** (reuse the server's running providers):
  rejected — N:1 means no single server to call, per-user credentials, and
  borrowing a user's model/budget to curate *shared* knowledge is wrong.
- **Bundle the generator in the embedder container:** rejected — different runtime
  (llama.cpp/GGUF vs sentence-transformers), different resource profile, and the
  embedder is required while the curator is optional.
- **Sub-10B for the whole curator:** rejected for Tier B (weak reasoning poisons
  the graph); kept for Tier A (mechanical + grammar-constrained, where small is
  genuinely a value-add).
- **Curation driven by the servers** (each server curates the shared kb with its
  own model): rejected — coordination, per-user-model inconsistency, cost
  attribution, and it re-introduces the per-user-credential coupling we are
  removing.

## Open questions

1. **Default Tier-A model** — **Gemma 4 E4B** (~4.5B effective, 128K ctx,
   strong grammar-constrained extraction, CPU/edge-friendly). Runtime fetch means
   no license gate, so the choice is purely about extraction quality vs
   throughput; alternatives to A/B against the *actual* curator schema (a one
   env-var swap): Qwen3.5-4B/2B, IBM Granite 4.1 3B, SmolLM3-3B.
2. **GPU build variant** (ROCm/CUDA) scope and whether it ships now or later.
3. **Packaging:** is `aimee-curator` a **separate plugin** (maximally decoupled /
   independently placeable) or a service in the existing combined plugin?
4. **Config migration:** clean retirement vs alias of the `kb_curator_*_command`
   sidecar hooks now that stages reference providers.

## Risks

- The shared-provider extraction (§1) is real surgery across `aimee-server`; must
  preserve all existing wire-format/credential behavior. Mitigate with a surface
  net before refactor.
- Runtime model fetch adds a first-boot network dependency and download (one
  time, then cached in the model volume). Mitigate: a persistent tier-bound cache
  volume, a long health `startPeriod`, and pre-populating the volume for
  air-gapped deploys. In exchange the published image stays small (runtime only).
- A misconfigured Tier-B provider must fail **loudly** (§4), never silently emit
  low-quality curation.
