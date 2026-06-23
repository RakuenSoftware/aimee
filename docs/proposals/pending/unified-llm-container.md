# Proposal: One unified `aimee-llm` container for all LLM use (local CPU/GPU · forward · external)

- **State:** draft — **rev 7** · **roundtable SIGNED OFF at rev 6** (round 5: 0
  blocking / 0 high / 0 medium, converged). Arc: r1 10 blocking → r2 7 blocking +
  8 high → r3 1 blocking + 1 high + 2 medium → r5 **0 blocking**. rev 7 closes the
  two open questions (synth models BENCHMARKED grammar-enforced + judged —
  Gemma-4-E4B CPU / Gemma-4-12B unified-GPU (Gemma-4-26B-A4B dedicated), Qwen3.6
  dropped on JSON-reliability, dense 31B dominated; decouples + shrinks the
  reranker to Ettin ModernBERT — 400m GPU / 68m CPU, both `-fa on`, Qwen3-Reranker
  dropped from the real-time path; E4B-ungated CPU default). See "Decisions
  taken" and "Changelog".
- **Implementation update (2026-06-23) — P2 `.254` Vulkan serving validation
  (see [`benchmarks/results/unified-llm/P2-serving-validation.md`](../../../benchmarks/results/unified-llm/P2-serving-validation.md)).**
  Two design refinements from empirical validation on the 7900XTX:
  1. **Reranker is served as ENCODER + a gateway-side Dense head, not native
     `/v1/rerank`.** `cross-encoder/ettin-reranker-{400m,68m}-v1` are
     sentence-transformers models whose score head (`2_Dense`→GELU→`3_LayerNorm`→
     `4_Dense`→1) does **not** survive `convert_hf_to_gguf.py` — a naive GGUF is
     encoder-only and misranks. The validated path: llama.cpp serves the ettin
     encoder (`/v1/embeddings --pooling cls` on `query</s>doc`), and the gateway
     applies the ~4 MB Dense head (pure numpy, no torch). Ettin reranks correctly +
     confidently this way (400m: relevant 8.9 vs irrelevant 1.1). So §2's "native
     ModernBERT `/v1/rerank`" is amended to "ettin encoder on llama.cpp + the
     gateway's reranker handler applies the head."
  2. **Models are BAKED into two images (`aimee-llm-cpu` / `aimee-llm-gpu`), not
     runtime-fetched** (operator-directed). This supersedes the
     embedder-runtime-fetch-once posture for this container: each image ships its
     tier's GGUFs (CPU: Qwen3-Emb-0.6B + ettin-68m encoder + head + gemma-E4B; GPU:
     Qwen3-Emb-4B + ettin-400m encoder + head + gemma-12B). Turnkey, reproducible,
     no HF dependency at runtime. The §5 install-time GPU **detection** still selects
     which image/tier; the four modes (local/forward/external) are unchanged.
  Embedder validated: Qwen3-Emb-0.6B→1024, 4B→2560 on Vulkan with the required
  serving flags (no `GGML_ASSERT` crash). Build pin: llama.cpp **b9761**.
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

| Install | Embed (GGUF) | dim | selection |
|---------|--------------|-----|-----------|
| CPU-only (**default**) | `Qwen/Qwen3-Embedding-0.6B-GGUF` | 1024 | auto |
| GPU (**default**) | `Qwen/Qwen3-Embedding-4B-GGUF` | 2560 | auto (any GPU install) |
| GPU (**opt-in**) | `Qwen/Qwen3-Embedding-8B-GGUF` | **4000** (trunc 4096→4000) | operator must explicitly configure |

> **Basis (2026-06-22, baseline-gated — supersedes every earlier embed-ladder note,
> including PR #613's "nomic-everywhere" and #617's capped-SciFact revert).** The
> embedder was re-validated end-to-end against **published** baselines; full data,
> the four-model ladder, and the harness are in
> [embedder-gate-scifact](../../validation/embedder-gate-scifact.md). Headline:
> - **aimee embeds code** (`kb_curator_index_code_unit.c` body/signature vectors,
>   `kb_service_code_embed.c` code chunks) through the same configured embedder, so
>   **code retrieval is the decisive axis**. **nomic-embed is text-only** (never
>   trained on code): on aimee's *own* 1864-function code set Qwen3-0.6B beats nomic
>   by **+12.7 nDCG@10 / +10 Recall@10**; on published MTEB code tasks the gap is
>   **+9 to +63**. **nomic is dropped.**
> - **Text is a wash-to-win for Qwen3.** Both reproduce their published BEIR numbers
>   within ~1pt (harness validated): they tie on SciFact (0.703 vs 0.706) and
>   NFCorpus, and Qwen3 wins the rest (FiQA +9, SCIDOCS +7, ArguAna +19,
>   TREC-COVID +27). The earlier "0.820 > 0.799 SciFact" was a **capped-corpus
>   artifact** and is withdrawn.
> - **Quality scales then plateaus.** aimee-code nDCG@10: 0.6B **0.697** → 4B
>   **0.759** → 8B **0.761**. The **0.6B→4B step is +6.2; 4B→8B is +0.16 (noise)** —
>   confirmed f16-vs-f16, and 4B-Q8 == 4B-f16 (quantization lossless here). So **4B
>   is the GPU default**: it delivers 8B-grade quality at **2560-d vs 4096-d**
>   (1.6× smaller pgvector index, indexed natively under the 4000-d `halfvec`
>   ceiling — no truncation), ~1.4× faster embed, ~½ the VRAM.
> - **8B is an operator opt-in**, not auto-selected by VRAM. It buys only the last
>   ~0.2 nDCG and costs a 4000-d (truncated) index; a deployment that wants it must
>   **explicitly configure** the 8B tier (see "The 8B truncation" below). 0.6B and
>   4B need no truncation.
>
> **Required serving config** (the proposal's `aimee-llm` must launch the embedder
> with these, or it crashes/slows on llama.cpp + Vulkan):
> `--ctx-size 8192 -ub 512 -np 1 --cache-ram 0 --no-cache-idle-slots` — the default
> prompt cache fragments the embedding KV cache (→ `GGML_ASSERT(task)` crash) and
> continuous batching across slots hangs the server. Keep `-ub` ≤ 2048 (RADV
> per-buffer limit). (The HTTP `/v1/embeddings` path returns one vector per input by
> construction; the `--no-escape` benchmark gotcha is CLI-only and does not affect
> the server.)
>
> **Still provisional for the full cutover:** this is embedder-isolated retrieval
> (BEIR text + aimee's own code), not yet the full-pipeline ship-floor gate
> (rerank + fusion vs the pplx baseline), which remains the ship precondition.

**Reranker — decoupled, not part of the embed ladder.** The reranker scores
`(query, candidate)` text pairs, so it is **dimension-agnostic** and need not scale
with the embedder. It runs **in the retrieval path of a user turn, so it must be
real-time** (<~1s for the candidate set). That requirement **rules out the
generative Qwen3-Reranker** — measured on the 7900XTX, its correct (yes/no-logit)
scoring is ~320 ms/candidate → **top-20 = 4.4s**, and llama.cpp's native
`/v1/rerank` scores it **incorrectly** (it is a yes/no causal LM, not a
classifier-head cross-encoder, so the rank-pooling path is invalid — verified: an
irrelevant doc scored highest).

The reranker is therefore **Ettin** (2025 ModernBERT cross-encoders — *already the
pplx/ettin baseline family*; llama.cpp has native ModernBERT support). It is
**correct *and* fast** via the native `/v1/rerank` endpoint, and is higher
quality-per-param than both Qwen3-Reranker and bge-reranker-v2-m3 (published
MTEB(eng, v2) nDCG@10: ettin-150m **0.599 > Qwen3-Reranker-0.6B 0.594**; ettin-32m
**0.578 > bge-reranker-v2-m3 0.553**; ettin-1b 0.611 = mxbai-rerank-large-v2).

> **Flash Attention is mandatory (`--flash-attn on`).** On this Vulkan build `-fa
> auto` does **not** engage FA; forcing it on **~2× the GPU throughput** (ettin-400m:
> 17 vs 36 ms/candidate). Every reranker `llama-server` launches with `-fa on`.

| Tier | Reranker (GGUF) | Latency (measured, 7900XTX, `-fa on`) | quality (nDCG@10) |
|------|-----------------|----------------------------------------|-------------------|
| **GPU (default)** | `ettin-reranker-400m` | top-20 **0.34s** (17 ms/cand), top-50 ~0.85s | ~0.605 |
| **CPU-only** | `ettin-reranker-68m` | top-10 **0.60s** (60 ms/cand); top-20 1.22s | ~0.589 |
| CPU strict top-20 <1s | `ettin-reranker-32m` | top-20 ~0.76s | ~0.578 |
| GPU max quality (opt-in) | `ettin-reranker-1b` | slower; = mxbai-large quality | ~0.611 |

Both default tiers run the **native `/v1/rerank`** endpoint — one relevance score
per `(query, candidate)` pair, **no chat template and no yes/no-logprob transform**
(that machinery was specific to the rejected Qwen3-Reranker). Qwen3-Reranker is
dropped from the real-time path; it remains usable only **async** (e.g. background
memory re-ranking) where its quality is worth seconds of latency.

**Synth ladder** (benchmarked grammar-enforced + judged on the curator's real
extraction task — see below):

| Install | Synth model (GGUF) | arch |
|---------|--------------------|------|
| CPU-only (**default**) | **`ggml-org/gemma-4-E4B-it-GGUF`** (Gemma 4 E4B — ungated mirror, no `HF_TOKEN`) | ~7B raw / E4B effective |
| GPU — unified (**default**) | **`unsloth/gemma-4-12b-it-GGUF`** (Gemma 4 12B) | 12B dense, ~8 GB |
| GPU — dedicated synth host | **`unsloth/gemma-4-26B-A4B-it-GGUF`** (Gemma 4 26B-A4B) | 26B MoE / ~4B active, ~17 GB |

> **Basis (2026-06-23, grammar-enforced + judged).** The curator does **async
> structured-JSON extraction** (`scripts/curator-extract.py`: doc/code → a schema'd
> `{"artifacts":[…]}` object). Throughput-bound (drains a queue), not latency-bound.
> The first pass was **confounded** — it scored `response_format=json_object`, which
> b9761 silently ignores, so it measured freeform-JSON luck. Redone with a
> **sampler-level GBNF grammar** (guaranteed valid JSON) over a **60-sample corpus
> drawn from the real ~/dev + ~/gow workspace** (every aimee-supported language),
> scored on three uniform layers: a strict de-saturated schema rubric, a corpus-wide
> valid-JSON rate, and a **blind Claude content judge**. Full data + harness in
> [`benchmarks/results/synth/RESULTS.md`](../../benchmarks/results/synth/RESULTS.md).
>
> Headline: under grammar enforcement **faithfulness is universal** — no model
> hallucinates; the differentiators are **structure** and **reliability**. The old
> "every Qwen slips" / "Granite wins" findings were harness artifacts. Corrected:
> - **GPU quality ties** across gemma-4 12B/26B-A4B/31B (all perfect schema +
>   valid-JSON). The pick splits on **deployment VRAM** because this container
>   co-hosts embed + rerank: **gemma-4-12B** (~8 GB, 53 tok/s) for the *unified* GPU;
>   **gemma-4-26B-A4B** (MoE, ~4B active → 84 tok/s, ~17 GB) when synth has a GPU to
>   itself. Dense **gemma-4-31B** is dominated (slower *and* larger).
> - **Qwen3.6 dropped on reliability** — content is strong but 8–12% of outputs
>   **truncate** even under grammar (valid-JSON 0.88–0.92) → ~1-in-10 lost extractions.
> - **CPU stays gemma-4-E4B.** A 2026 ≤4B bake-off (granite-4.0-micro/h-micro,
>   nemotron-3-nano-4b, phi-4-mini, smollm3-3b, lfm2.5-1.2b/8b-a1b) found **none beats
>   it** — each trades gemma's one weakness (it flattens `doc_summary`) for a worse one
>   (placeholder-echoed or malformed code, or sub-0.4 doc). E4B keeps perfect code +
>   100% valid JSON. `smollm3-3b` is the alternative for doc-heavy / code-light hosts.
>
> Pin each to a specific **commit sha + quant**.

#### The 8B truncation — why 4000, and how it must be done

> **Scope (2026-06-22):** the **GPU default is Qwen3-4B at 2560-d, which needs no
> truncation** (it is under the 4000-d `halfvec` ceiling). This 4096→4000 machinery
> applies **only when an operator opts into the 8B tier** (see the embed-ladder
> "Basis" above — 8B buys ~0.2 nDCG over 4B for a 4096-d native vector). It is kept
> because the 8B opt-in must store an indexable vector.

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

A dim-only guard is insufficient — two different models can share a dim:
`pplx-embed` and the default `Qwen3-Embedding-0.6B` are **both 1024-d**, so a
dim-only check would silently mix incompatible spaces on that exact swap. The
guard therefore records and compares **embedder model identity
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

The reranker mapping is **pinned, not deferred**: Ettin runs as a ModernBERT
cross-encoder over the **native `/v1/rerank`** endpoint with **`--flash-attn on`** —
one relevance score per `(query, candidate)` pair, **no chat template and no
yes/no-logprob transform** (that machinery was specific to the rejected generative
Qwen3-Reranker and is removed). The scoring contract the drift guard pins is
therefore `(model_id, rerank-endpoint=/v1/rerank, fa=on)`. Two **distinct** quality
gates (do not conflate):

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

- **Embed = `Qwen3-Embedding-0.6B` CPU default (1024-d), `Qwen3-Embedding-4B` GPU
  default (2560-d), `Qwen3-Embedding-8B` operator opt-in (4000-d trunc).** Baseline-
  gated against published numbers — see embedder-gate-scifact. Decisive axis is
  **code** (aimee embeds raw code): nomic is text-only and loses by +9..+63 nDCG on
  code (and +12.7 on aimee's own code), so **nomic is dropped**. On aimee-code the
  ladder is 0.6B 0.697 → 4B 0.759 → 8B 0.761, i.e. **4B≈8B (+0.16, noise)** so 4B is
  the GPU default (8B's 4096-d isn't worth ~0.2 nDCG); 8B is opt-in only. Both the
  earlier "nomic-everywhere" (#613, LoCoMo) and the capped-SciFact "0.883/0.820/0.799"
  revert (#617) are **withdrawn** as wrong/artifactual. Serving must run with
  `-np 1 --cache-ram 0 --no-cache-idle-slots` (else the prompt cache fragments the
  embedding KV → `GGML_ASSERT(task)` crash). Full-pipeline ship-floor gate still
  pending.
- **Curator fold: take it** as an isolated supervised process + RBAC; off-box
  synth via forward/external; sidecar fallback = kernel-compromise response.
- **Per-user synth via the gateway** with §1a hardening; **escape hatch** is an
  off-by-default operator flag that still audits.
- **Drift guard → `(model_id, dim)` for embedder and reranker** + admission-gated
  compat-list.
- **Auth default = mTLS**; bearer dev-only.
- **CPU synth default = Gemma 4 E4B** via the ungated `ggml-org` GGUF mirror (no
  `HF_TOKEN`); the separate "ungated fallback" model is dropped.
- **Synth models — benchmarked grammar-enforced + judged** (2026-06-23; the
  2026-06-22 pass was confounded by an ignored `json_object` and is superseded):
  **CPU = Gemma 4 E4B** (best ≤4B vs the 2026 field), **GPU = Gemma 4 12B** for the
  unified container (~8 GB, co-resides with embed+rerank) / **Gemma 4 26B-A4B** (MoE)
  for a dedicated synth host. Under grammar enforcement faithfulness is universal;
  **Qwen3.6 dropped on reliability** (8–12% truncation), **dense Gemma-4-31B dominated**
  (slower + larger than the 26B MoE at equal quality). Replaces the rev-7 Gemma-12B/26B
  + Qwen3.6 ladder. See `benchmarks/results/synth/RESULTS.md`. **Reranker = Ettin (ModernBERT
  cross-encoder), NOT Qwen3-Reranker**: GPU default `ettin-reranker-400m`, CPU-only
  `ettin-reranker-68m`, both `--flash-attn on`. Real-time on the 7900XTX (ettin-400m
  top-20 0.34s; ettin-68m top-10 0.60s) and correct/fast via native `/v1/rerank` —
  Qwen3-Reranker is generative (~320 ms/cand, top-20 4.4s) and llama.cpp's native
  rerank scores it wrong, so it is dropped from the real-time path. The reranker
  guard is keyed on `(model_id, scoring-contract)`, not dim.
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
  benchmark (re-run grammar-enforced + judged 2026-06-23; the 2026-06-22 pass was
  confounded by an ignored `json_object`) — **CPU = Gemma 4 E4B** (ungated `ggml-org`
  mirror → no `HF_TOKEN`; rev-5 "ungated fallback" dropped; best ≤4B vs the 2026 field),
  **GPU = Gemma 4 12B** (unified) / **Gemma 4 26B-A4B** (dedicated synth host); replaces
  the rev-7 Gemma-12B/26B + Qwen3.6 ladder (Qwen dropped on JSON-reliability, dense 31B
  dominated). **Decoupled the
  reranker** from the embed ladder (it is dimension-agnostic): **Ettin ModernBERT
  cross-encoder — `ettin-reranker-400m` (GPU) / `ettin-reranker-68m` (CPU), both
  `-fa on`; Qwen3-Reranker dropped from the real-time path** (too slow + native
  rerank scores it wrong).
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
