# Proposal: ingress cost-accounting coverage + request-level cost optimizations

- **State:** reviewed — design-ready (2026-06-16). §1–§2 foundations landed
  (#185/#339); the design roundtable's 12 blockers on the remaining §2–§7 work are
  resolved in §8. Implementation-ready with **no external dependency**: delegates are
  subscription-priced (per-token cost 0, the #339 default) and a metered delegate's
  price is operator-configurable on its model via the model_registry
  `cost_in/out_per_mtok` override (see §8). (Consolidated after PR #180 review +
  twelve file-by-file codebase audits; findings integrated below.)
- **Implementation status (2026-06-16):** §1–§2 LARGELY LANDED, design-roundtable
  run on the rest. **Merged:** §1 (one pricing authority — `token_estimate_cost`
  + registry-fallback hook, `token_estimate_cost_ex` is_priced free-vs-unknown,
  `delegate_ensemble` routed through it; static table reconciled with the model
  registry) and the §2 **schema/foundations** (`usage_kind`
  realized/estimated/avoided/partial + realized-only filter + spend-breakdown,
  `requested_model`/`stop_reason`/`agent_log_id`, billable-model resolution,
  `request_context.h`) shipped in **PR #185**; the §1 local-delegate pricing
  coverage gap (minimax/mistral/mimo as known-zero) closed in **PR #339**.
  **Design roundtable (2026-06-16):** ran twice; the second pass enumerated 12
  blockers on the remaining §2/§3–§7 work — **all now RESOLVED in §8** (the key
  decision: v1 audit writes are synchronous, dissolving the async-queue + reward-
  barrier blocker class; dedup fail-closed excludes shared principals; HMAC-bound
  proxy credential; enumerated dedup-key allowlist; scoped thread-local reset;
  capability-gated flag). **Remaining (implementation, now unblocked):** §2
  ingress-writes to the six no-log handlers, §3 cache-aware shaping, §4 dedup, §5
  reasoning-effort cap (still recommend defer), §6 cost-shaped reward, §7
  `/v1/usage/*`. **No external dependency:** delegate pricing is per-delegate
  configurable on its model via the model_registry `cost_in/out_per_mtok` override,
  defaulting to 0 for subscription delegates (see §8).
- **Author:** JBailes
- **Date:** 2026-06-11
- **Charter roles:** Evaluate-Optimize (cost-shaped reward into the existing
  bandit), Calibrate (cache/thinking thresholds), Recall (cache-aware ingress
  shaping only where the ingress already owns the prompt), Gate-Promote
  (default-off flag rollout per the readiness program).
- **Scope:** `src/server/anthropic_http.c` (observe provider usage in the native
  Anthropic relay path + buffered path; resolve the billable model),
  `src/server/anthropic_ingress.c` (`anthropic_stream_feed_openai` - capture full
  normalized OpenAI-stream usage, not just `completion_tokens`),
  `src/server/openai_chat.c` (the live OpenAI/Codex inference and pre-injection
  seam; direct ingress audit rows for chat/completions, completions, responses,
  streaming responses, and runs),
  `src/server/agent_runtime.c` (`agent_log_call` - fix model attribution so rows
  resolve a real billable model), `src/server/token_tracker.c` +
  `src/headers/token_tracker.h` (single pricing source of truth, reconciled with
  the model registry), `src/headers/model_registry.h` (`cost_in_per_mtok` /
  `cost_out_per_mtok` - the authoritative base-price fields),
  `src/db1/token_audit.c` + `src/db1/schema.sql` (the existing audit ledger plus
  any required migration fields), `src/db1/agent_log.c` + `src/db1/agent_log.h`
  (agent metrics currently join token_audit by lossy agent/role keys),
  `src/server/ingress_preinject.c` (envelope
  metadata/splitting only if needed by the OpenAI/Codex seam),
  `src/server_insights.inc` (the `insights.overview` implementation),
  `src/cmd_core.c` (`cmd_usage`), `src/server/server_compute.c` (cost-shaped
  reward on the existing `delegate_routing` bandit close), `src/server/server_http.c`
  + `src/headers/server_http.h` + `src/server/server_auth.c` (request-context
  propagation + UDS peer-UID capture for source, idempotency, principal, and
  request-id metadata), `api/openapi-server-v1.yaml`,
  `src/server/openapi_server_data.h`, `src/cli_rpc_routes.inc`,
  `webchat/openai.go` and `webchat/socket.go` (webchat proxy/session source attribution),
  `src/cmd_agent.c`, dashboard/HUD/frontend readers that consume token-audit totals, config
  plumbing (`src/headers/config.h`,
  `src/config_fields.c`, `src/config_sections.c`, `src/config_save.c`), route and
  auth registration only if a new API method is still justified. DB2 only if a
  shared/multi-machine analytics need is
  established (§2). Unit + integration tests. No new service, no new model.

## Revision note

This is a heavily revised draft. v1 proposed new machinery — a `cost_pricing.{c,h}`
module, a DB2 usage ledger, an `aimee usage` CLI, and raw-dollar bandit rewards.
The PR #180 review and a series of file-by-file codebase audits showed aimee
**already ships** the core machinery: cache-aware pricing (`token_tracker.c`,
`token_estimate_cost`), an audit ledger with most of the needed schema
(`db1/token_audit.c`), a reader (`cmd_usage` → `db1_token_audit_*`), a usage/cost
summary route (`insights.overview`), and registry price fields
(`model_registry.h`). The objective is therefore **coverage and correctness of the
existing machinery for ingress requests**, not new accounting.

Two framing corrections from the audits underpin everything below: Anthropic
`/v1/messages` is intentionally a stateless proxy and is **not** a pre-injection
surface (the shipped seam is `openai_chat.c`), and the `<aimee-context>` envelope
is per-turn query-derived, so it is **not** an inherently stable cache prefix.

### Findings map

The audits produced 23 verified, in-tree findings; they are grouped by theme here
and carried with file:line evidence in §0. The `(N)` numbers are stable
references used elsewhere in this doc.

- **Pricing (§1).** (1) Pricing lives in **four** divergent sources —
  `token_tracker.c` (substring match, the only one with cache prices), the model
  registry (exact match, no cache prices), `delegate_ensemble.c` (a third
  flat-rate calculator), and `models_dev_snapshot.json` — so the same model id can
  be priced differently by different paths.
- **Audit schema & migration (§2).** (2) `by_model` filters out every empty-model
  row, so the model-write fix and the filter/backfill must land together or
  history shows a false zero. (11) Idempotency needs a UNIQUE index the additive
  reconcile will not create, and SQLite cannot add a `NOT NULL` column without a
  default. (15) `agent_log`↔`token_audit` is joined by agent-name/role — an N×M
  join that multiplies cost/cache sums — so a shared `call_id` is required.
- **Ingress write coverage (§2).** (5) The six synchronous OpenAI/Codex handlers
  go through `agent_execute()` and write **no** audit row today; (8) but `/v1/runs`
  already logs (detached worker → `agent_log_call`), so it needs attribution
  **only**, never a second write (double-count guard). (4)
  `stream_options.include_usage` is set nowhere and the OpenAI buffered parser
  drops cache tokens. (14) `/v1/embeddings` is a local $0 embedder → exclude from
  spend by default. (22) aimee-kb-side LLM spend (`/v1/rules/generate`, curator
  extraction) runs in a **separate process** outside this ledger → out of scope.
  (23) OpenAI/Codex streaming is **compute-then-chunk**, so it writes realized
  spend on provider return; only provider-incremental streams (native Anthropic
  relay) can be `partial`.
- **Request context & threading (§0/§2).** (6) HTTP handlers receive only
  `(body, resp, cap)` — no source/principal/idempotency/request-id. (9) A
  thread-local context breaks for the detached `/v1/runs` pthread → capture into
  the job struct at enqueue. (10) `session_id()` is PPID-process-wide, so ingress
  source must come from the request context, not `session_id()`.
- **Identity & trust (§0).** (13) `webchat/openai.go::proxyV1` strips
  `Authorization` over the UDS and stamps no replacement identity. (16/18) The
  HTTP-over-UDS path never captures `SO_PEERCRED` and grants `CAPS_ALL` to every
  UDS peer, so peer-UID capture and a trusted-peer mechanism are **net-new** code.
  (19) A UID allowlist is insufficient in a same-user local deploy (though the
  container deploy runs webchat as root vs the server as uid 1000); the robust
  answer is a proxy credential, reusing the existing `$AIMEE_HOME/server.token`
  pattern.
- **Cache shaping (§3).** (3) The aimee-owned outbound Anthropic `system` is a
  **flat string**, so placing `cache_control` on a stable block requires
  converting it to a content-block array — the real work, not a flag.
- **Storage / concurrency (§0).** (17) DB1 is a single shared SQLite handle and the
  audit insert is `(void)` best-effort, so under the added per-turn write volume a
  WAL busy-handler exhaustion can **silently drop** a row (undercount) → prefer an
  async/batched off-thread write and load-test drop rate + tail latency.
- **Readers & API contract (§7).** (7/12) Eight consumers sum `estimated_cost_usd`
  as scalar spend (incl. `server_compute.c` cost-fold and the agent-log join) and
  must learn `usage_kind` semantics; `Chat.tsx` does not price client-side, so §1
  propagates to it for free. (20) `insights.overview` is OpenAPI `type: object`,
  served from a generated header, with a thin-client printer that shows only
  `estimated_cost_usd` — all three must learn the new fields. (21) Webchat's
  browser usage SSE `{in, out, cost}` is a separate, undocumented contract.

## Goal

Ingress requests are a cost blind spot. Normal agent and delegate calls are
audited (`agent_log_call` → `token_audit`), but requests arriving on aimee's
**Anthropic ingress** (`POST /v1/messages`) and **OpenAI-compatible ingress** are
not consistently folded in. This proposal (a) extends the existing audit to cover
ingress turns with a correct billable model and source, and (b) adds a small set
of request-level optimizations — cache-aware shaping first — wired into the
optimization surface aimee already has, not a new one.

## §0 What already exists, corrected

- **Pricing is split across four sources, and they disagree.** A full audit
  found more divergence than "two sources":
  1. `token_tracker.c:17-47` — a 17-row table with the **only** cache-price
     fields, matched by case-insensitive **substring**, first-match-wins
     (`find_price` / `token_strcasestr_local`, `:56-85`).
  2. `model_registry` — `cost_in_per_mtok` / `cost_out_per_mtok`
     (`model_registry.h:39-40`), a 9-row static table (`model_registry.c:439-469`)
     plus a dynamic models.dev cache and an override JSON, matched by **exact**
     provider-qualified `strcasecmp`, with **no cache-price fields**.
  3. `delegate_ensemble.c:88-99` (`model_token_cost`) — uses the registry and
     falls back to a flat `0.000015`/token (`:21`), **bypassing token_tracker**,
     so ensemble cost silently loses Anthropic cache pricing.
  4. `data/models_dev_snapshot.json` (10 models, `inputCost`/`outputCost`, no
     cache) loaded into the registry via `models_dev_cache.c`.
  Because token_tracker is substring and the registry is exact, the **same model
  id can be priced differently by different code paths**, and their coverage
  differs (17 vs 9 models). The substring matcher is also a mispricing vector
  (table-order dependence; an adversarial/compound id can match an Anthropic
  price). §1 collapses these to one authority and adds a drift test.
- **The audit ledger already covers the basic counters.** `db1/token_audit.c`
  inserts `(session_id, delegation_id, project_name, tool_name, role, model,
  prompt_tokens, completion_tokens, cache_write_tokens, cache_read_tokens,
  estimated_cost_usd)` and exposes totals / by-role / by-tool / by-model /
  by-delegation aggregations. `cmd_usage` (`cmd_core.c`) and `insights.overview`
  (`src/server_insights.inc`) already read it. This is DB1 (SQLite), local.
- **The audit ledger does not yet encode all semantics this proposal needs.** It
  has no explicit source/client, no usage-kind (`realized`, `estimated`,
  `avoided`), no requested-vs-served model, no duration/stop reason, and no
  optimization metadata. Do not smuggle all of that through `role`/`tool_name` if
  downstream dashboards need to distinguish it. §2 defines the minimum migration.
- **The streaming translator does NOT see full Anthropic usage** (this was the
  draft's biggest error). `anthropic_http.c::messages_stream` has **two** paths:
  native Anthropic providers are relayed byte-for-byte through
  `anthropic_relay_chunk_cb` and **never** touch `anthropic_stream_xlate_t`; the
  translator is the OpenAI-style path only. And
  `anthropic_ingress.c::anthropic_stream_feed_openai` reads **only**
  `usage.completion_tokens` — no input, no cache tokens, no Anthropic
  `message_delta.usage`. So "the provider's authoritative usage already passes
  through the translator" was false. §2 must add the observation.
- **`count_tokens` is an estimate, not spend.** It uses
  `session_compact_estimate_tokens()` locally and never calls a provider. It must
  not write a spend row.
- **The live pre-injection surface is not Anthropic Messages ingress.**
  `docs/proposals/done/context-preinjection-ingress.md` states this explicitly:
  the Codex/OpenAI handlers inject the envelope, while Anthropic `/v1/messages`
  stays a pure stateless proxy by design. The live call sites are in
  `src/server/openai_chat.c`, with `ingress_preinject_build()` and sometimes
  `ingress_preinject_apply()`. Anthropic support would be a separate opt-in
  mutation phase, not a free cache-marking change in `anthropic_http.c`.
- **The `<aimee-context>` envelope is not inherently stable.**
  `ingress_preinject_build(query, ...)` rebuilds from the turn query, code
  search, memory context, and fresh audit context. Cache marking must separate
  stable prefix material from volatile per-turn retrieval material or prove
  stability by realized cache-read measurements. Do not mark the whole envelope
  as a stable cache anchor by assertion.
- **The bandit reward is a scalar in [0,1].** `server_compute.c` closes
  `delegate_routing` with `rc == 0 ? 1.0 : 0.0`, and `kb_client_bandit_close`
  documents reward as `[0,1]` over beta-style posteriors. Raw dollars cannot be
  the reward (§6).
- **`agent_log_call` mis-attributes the model, and `by_model` hides the
  fallout.** It estimates cost from `result->agent_name` (`agent_runtime.c:1912`)
  and writes an **empty** `model` (`:1926`); and `anthropic_http.c:163-165` reads
  the client's *requested* model but routes to the configured primary
  (`resolve_primary`). Crucially, `db1_token_audit_by_model`
  (`token_audit.c:243,253`) filters `WHERE COALESCE(model,'') != ''`, so **every
  existing row is already excluded** from by-model. If ingress rows start writing
  real models while legacy rows stay empty, the by-model surface shows a
  **false-zero history** — recent ingress spend appears while all prior spend
  vanishes. The model write-fix and the filter/backfill must land together (§2).
- **Most OpenAI/Codex ingress bypasses `agent_log_call` — but `/v1/runs` does
  NOT, and that asymmetry is a double-count trap.** `agent_execute()`
  intentionally does **not** log (`agent_runtime.c:1092-1095`: "callers … handle
  logging … Do not log here to avoid double-logging"), and `openai_chat.c` calls
  it (or `agent_execute_messages()`, a one-step helper that also never logs)
  directly for the **six** synchronous handlers: buffered + streaming
  `/v1/chat/completions`, buffered + streaming `/v1/completions`, buffered
  `/v1/responses`, and streaming `/v1/responses`. Those write **no** `token_audit`
  row today. **`/v1/runs` is different**: `runs_handler` spawns a detached worker
  (`pthread_create` + `pthread_detach`, `openai_chat.c:1156-1164`) that calls
  `agent_run_with_tools()` → `agent_log_call()` (`agent_runtime.c:406`), so it
  **already writes a row**. Therefore the implementation must add new writes only
  to the six no-log handlers; for `/v1/runs` it must add **attribution only**, not
  a second write, or it double-counts. The general rule: never add an ingress
  write to any path that already flows through `agent_run*` / `agent_log_call`.
- **The HTTP ingress handlers cannot currently see the metadata this proposal
  needs.** `server_http_completion_fn` and stream handler typedefs receive only
  `(body, resp, cap)` or `(body, emit, ctx)`; `Authorization`, `x-api-key`,
  `X-Aimee-Session-Key`, idempotency headers, request id, route path, TCP-vs-UDS,
  and derived bearer capabilities are consumed in `server_http.c:1422-1451`
  (a `request_id` is even generated, `:1424-1430`) and then discarded — only
  logged, never passed to handlers. Source attribution, account isolation,
  idempotent dedup, and audit row identity therefore require widening the handler
  signatures (or a request context) before any ingress writer lands.
- **Source/principal erasure over UDS is systemic, not webchat-specific.** The
  audit confirmed `webchat/openai.go::proxyV1` validates the webchat bearer, then
  `req.Header.Del("Authorization")` (`:84`) and forwards `/v1/chat/completions`,
  `/v1/completions`, `/v1/responses`, `/v1/embeddings` over the UDS with **no**
  replacement headers — webchat HAS identity it could stamp (PAM username,
  `aimee_session_id`, `attach_id`) but forwards none. But the deeper fact is that
  **every** UDS HTTP client is indistinguishable: `server_http_authorize` returns
  0 for `!is_tcp` ("UDS: filesystem-permission auth, no token") and
  `server_http_conn_caps` grants `CAPS_ALL`, so the thin CLI, MCP, and webchat all
  arrive as anonymous trusted-local with no caller identity. Notably the **HTTP-
  over-UDS path captures no peer identity, whereas the legacy NDJSON socket already
  derives `uid:<peer_uid>`** (`server_auth.c:263-265`). Two complementary fixes:
  (a) capture the UDS peer UID on accept as a baseline principal (parity with
  NDJSON), and (b) require forwarding proxies — webchat first — to stamp trusted
  internal headers for source, principal/session, original request id, and
  idempotency. Attribution cannot rely on the forwarded HTTP request alone. **Both
  are net-new code:** `handle_conn` never calls `platform_ipc_peer_cred()` today
  (it exists but is uncalled on the HTTP path), and there is no trusted-peer
  allowlist — so distinguishing a trusted proxy (webchat) from an arbitrary UDS
  peer, and gating forwarded-header acceptance on it, must be built before any
  forwarded source/principal is trusted (it is a security prerequisite, not a
  config knob). A UID allowlist by itself is insufficient in a same-user local
  deploy (proxy and arbitrary clients share a UID) — though in the container
  deploy webchat is root and the server is uid 1000, so UID *can* separate them
  there. The robust cross-deploy answer is a proxy credential, and aimee already
  ships the pattern: webchat reads `$AIMEE_HOME/server.token`, so reuse that
  shared-secret/HMAC rather than invent a new factor.
- **DB1 is a single shared SQLite handle, and audit writes are best-effort.** DB1
  is one process-wide `sqlite3*` (`db1_init.c:19`) opened `SQLITE_OPEN_FULLMUTEX`
  with `journal_mode=WAL` and a busy handler that retries 15× (20–150 ms) then
  **gives up** (`:22-68`). Every HTTP connection already runs on a detached worker
  thread and `/v1/runs` on its own pthread, so concurrent `token_audit` writes
  happen today; the insert is `(void)`-ignored, so a busy-handler exhaustion
  **silently drops** the row. Adding a synchronous per-turn insert to the six
  streaming/buffered hot paths raises both contention (→ dropped rows = undercount)
  and tail latency. The accounting design must (i) state best-effort semantics
  explicitly, (ii) prefer an **async/batched** audit-write queue off the request
  thread, and (iii) load-test drop rate + tail latency before flipping the
  accounting flag on.
- **A thread-local context is NOT sufficient — it breaks for `/v1/runs`.** The
  cited precedent `ingress_preinject_set_request_disabled()` is `static __thread`
  (`ingress_preinject.c:28`) and works **only because the turn runs synchronously
  on the request thread**. The six synchronous handlers above keep that property,
  so a thread-local context would reach them. But the `/v1/runs` worker runs on a
  **detached pthread** that does not inherit the request thread's TLS, and SSE
  paths may offload via `sse_offload` — so any offloaded path must have its
  context **captured into the job/work struct at enqueue**, not read from a
  thread-local. The proposal's "survive offloaded workers" requirement is only met
  by capture-at-enqueue.
- **`session_id()` is process-wide for ingress, so per-source attribution cannot
  use it.** `session_id()` keys off the server's PPID (`config.c`), so **every**
  HTTP ingress request resolves to the **same** shared id. Consequences: ingress
  rows all collapse into one row in `db1_insights_top_sessions` (which JOINs
  `token_audit.session_id`), and `cost_for_delegation` only separates by
  `delegation_id`. The new `source` (and any per-client attribution) must derive
  from the **request context** (`X-Aimee-Session-Key` / principal), not from
  `session_id()`; otherwise top-sessions and per-source spend are meaningless for
  ingress.
- **Existing usage readers assume every row is spend.** The audit
  originally found eight consumers that sum `estimated_cost_usd` (or token-audit totals)
  with no `usage_kind` filter: `src/hud.c`, `src/cmd_core.c` (`cmd_usage`),
  `src/dashboard.c`, `src/server/dashboard_server.c`, `src/server_insights.inc`,
  `src/server/server_compute.c:1469` (the cost-fold query),
  `webchat/socket.go` (`streamEvent.Cost`), and `frontend/src/pages/Chat.tsx`.
- **Agent stats are a separate cost-reporting path, and the join is already
  lossy.** `db1_agent_log_metrics_by_role` and `db1_agent_log_agent_stats`
  (`src/db1/agent_log.c`) join `agent_log` to `token_audit` on `tool_name =
  agent_name` plus role, or on agent name alone. That is not a one-row-per-call
  relationship: multiple agent_log rows join every matching token_audit row, so
  cost/cache totals can be multiplied, and future ingress rows with no
  corresponding `agent_log` row either disappear from agent stats or contaminate
  an agent bucket by name. This reaches `cmd_agent` through `agent_get_stats`.
  The accounting migration must add a stable call key (`agent_log_id` or
  `call_id`) shared by both tables, or remove cost aggregation from agent_log
  joins and read spend only from token_audit's source-aware summaries.
- Once `usage_kind=estimated|avoided|partial` exists, all direct readers (plus
  the eight SQL aggregations in `token_audit.c`, the agent_log joins above, and
  CLI/frontend surfaces that display them) must split realized spend from
  advisory/avoided values, or the UI shows avoided cost as money spent. One
  upside: `Chat.tsx` only *displays* a server-sent `cost` (it does **not** compute
  price client-side), so unifying pricing server-side (§1) propagates to the UI
  with no frontend change.

## §1 One pricing source of truth (extend, don't add)

- **Pick one authority.** Either move the cache-aware multipliers into the model
  registry (add `cache_read_per_mtok` / `cache_write_per_mtok` beside
  `cost_in_per_mtok` / `cost_out_per_mtok`), or make `token_estimate_cost()`
  consume registry base prices and keep only the cache multipliers in
  `token_tracker`. Recommended: **registry is authoritative for base prices,
  `token_tracker` owns cache multipliers and the `token_usage_t` normalization** —
  one lookup, no third table.
- `token_estimate_cost()` keeps its current signature and the "0.0 on unknown
  model" contract; add a unit-tested registry fallback path. If substring
  matching remains as a fallback, test ambiguous names explicitly (`gpt-4o-mini`
  before `gpt-4o`, `o3-mini` before `o3`, etc.).
- Pin a dated `pricing_refreshed` marker in whichever file becomes authoritative
  so price drift is auditable.
- Treat provider-reported model aliases as normalization inputs. Cost lookup
  should use the billable provider model after alias resolution, not an agent
  nickname or a user-requested model string.
- **Route `delegate_ensemble.c` through the same lookup.** Its `model_token_cost`
  is a third calculator that bypasses token_tracker and flat-rates unknowns at
  `0.000015`/token; once one authority exists, ensemble cost must use it so it
  picks up cache pricing and so §6's "realized delegate cost" is consistent.
- **Add a drift test, not just ambiguity tests.** Beyond ordering cases
  (`gpt-4o-mini` before `gpt-4o`), assert token_tracker and the registry agree on
  price for every model both know — substring-vs-exact divergence is the actual
  bug, and a compound/adversarial id must not match an Anthropic price.
- **Disambiguate zero-price from free.** The registry lists `minimax` at
  `0.0/0.0`, which the ensemble path treats as "unknown" and silently flat-rates.
  Use an explicit `is_priced` signal (or sentinel) so a real zero never falls
  through, and close the coverage gap for the delegates aimee actually runs
  (minimax / mimo-2.5 / mistral) plus the o-series / gemini rows the registry and
  snapshot omit today.

## §2 Cover ingress turns in the existing audit (token_audit), not a new ledger

Write ingress turns into `token_audit` from the points that can observe real
provider usage, distinguishing **billable realized usage** from **local
estimates** and **avoided estimated cost**.

- **Minimum schema migration:** add fields (or a tightly-linked extension table)
  for `source`, `usage_kind`, `request_id` or `turn_id`, `requested_model`,
  `served_model` or `billable_model`, `provider_model`, `duration_ms`,
  `stop_reason`, a stable `call_id` / `agent_log_id` for rows that correspond to
  an `agent_log` call, and `optimizations_json` / `avoided_cost_usd` if
  dedup/cache savings are reported. If the decision is to avoid migration, state
  exactly how each field is represented and which reports lose fidelity. Two migration
  constraints the audit surfaced: `db1_reconcile_columns` (`db_schema.c:78`)
  **only adds columns** (no index, no data migration), and SQLite cannot
  `ALTER TABLE ADD COLUMN` a `NOT NULL` column without a default — so every new
  column must ship with a default, and the empty-model backfill and any index are
  **separate explicit steps**, not part of the additive reconcile.
- **Row identity / idempotency (and its migration cost).** Give each attempted
  provider call a stable request/turn id and make successful insertion idempotent
  on `(source, request_id, attempt)` — required for retries, background `/v1/runs`,
  and stream-abort cleanup. **But idempotency needs a UNIQUE index, which the
  additive reconcile does not create** (there is none on `token_audit` today, and
  db1's indexes are only created at table-creation in `schema.sql`). Resolve the
  contradiction explicitly: either add a one-off index-creation migration step, or
  enforce idempotency in-process (a seen-set keyed by request id) and accept that
  a crash between provider-return and insert can drop — not duplicate — a row.
  Do not use caller-controlled `X-Request-ID` alone as this key; keep it as
  external correlation metadata and derive audit idempotency from an explicit
  idempotency key, a server-generated attempt id, and the source/account boundary.
- **Request context prerequisite:** before adding source-aware audit rows or
  dedup, expose a per-request context from `server_http.c` to registered handlers:
  method/path, generated `X-Request-ID`, inbound idempotency key, session key,
  remote/local transport, bearer scope/principal (or local UID), and selected
  route capability. The thread-local preinject override is a precedent for the
  **synchronous** handlers only; because the `/v1/runs` worker is a detached
  pthread (and SSE may offload), the context for those paths must be **captured
  into the job/work struct at enqueue**, and the thread-local must be reset per
  request so it never leaks across reused worker threads. Forwarded identity
  headers must be accepted only from authenticated internal proxies and ignored
  or stripped on TCP and untrusted UDS requests, so external callers cannot spoof
  source or principal. Captured peer UID is the fallback principal; it is not
  enough to authenticate a proxy's forwarded user/session identity.
- **Fix `by_model` alongside the model write (prerequisite, not optional).**
  Populating `served_model` is pointless while `db1_token_audit_by_model` filters
  empty models out: either backfill legacy rows' model from their `tool_name`
  (the agent name, which is the model alias today) or relabel empty-model rows as
  `"(unattributed)"` instead of dropping them, so enabling ingress accounting
  does not make historical spend appear to vanish.
- **Native Anthropic relay path:** add a usage-observing tap to
  `anthropic_relay_chunk_cb` (or route native streams through a thin
  usage-observing relay) that parses `message_start` / `message_delta.usage`
  (input, output, `cache_read_input_tokens`, `cache_creation_input_tokens`) while
  still relaying the original bytes unchanged. Write one realized `token_audit`
  row at stream finish.
- **OpenAI-style streaming path:** request `stream_options: {include_usage:true}`
  only for providers known to support it; unsupported OpenAI-compatible providers
  must continue to work. Extend `anthropic_stream_feed_openai` state to hold full
  normalized `token_usage_t` (input + cache + output), not just
  `completion_tokens`. The provider request builder has to insert the option, not
  only the parser.
- **OpenAI/Codex direct ingress paths:** add one audit write for each successful
  provider call in the **six no-log handlers**: buffered + streaming
  `/v1/chat/completions`, buffered + streaming `/v1/completions`, buffered
  `/v1/responses`, and streaming `/v1/responses` (`agent_execute_messages()`
  result). **Do NOT add a write to `/v1/runs`** — its detached worker already
  logs through `agent_run_with_tools()` → `agent_log_call()`; it needs the
  source/attribution fix only, fed by the context captured at enqueue. And do not
  add logging inside `agent_execute()` itself; normal `agent_run*` paths already
  call `agent_log_call()`. Net: new writes only where no row exists today. Keep
  `/v1/embeddings` out of this LLM list unless embedding provider spend is
  intentionally added as a separate usage kind.
- **Embeddings ingress costs $0 today — default to excluding it.**
  `embeddings_handler` (`openai_chat.c`) calls `memory_embed_text`, which is
  **local**: the builtin hash embedder (`memory_embed_text_builtin`) or a local
  `embedding_command` via `platform_exec_pipe` — no paid API, and the embedder
  model id is absent from every price table (so `token_estimate_cost` → 0.0
  regardless). Its `usage.prompt_tokens` is itself a `chars/4` **estimate**, not
  provider-reported. So the correct default is **exclude embeddings from spend**,
  not "record as estimate-only." A remote-embedder cost hook is future-only — the
  `embedding_endpoint` / `embedding_model` config fields exist but are **unused**
  by `memory_embed_text` today; only if a remote embedder is wired does an
  explicit embedding usage-kind + embedder pricing source become real. The non-LLM
  ingress surface is **bounded**: the only other non-chat route is `/v1/models`
  (no usage). Never let embedding tokens inflate LLM by-model cost.
- **Buffered path:** parse the provider JSON `usage` block after a 200 response
  and before translating it back to the client. The Anthropic parser already
  extracts cache tokens (`agent_bridge.c:791-796`), but the **OpenAI buffered
  parser drops them** — this is an *add extraction* task, not just "verify they
  survive normalization."
- **`count_tokens`:** never writes a realized spend row. If exposed in usage
  summaries, it must be `usage_kind=estimated` and excluded from spend totals by
  default.
- **Failure / aborted streams:** no realized-spend row on provider error. A
  provider-incremental stream cut before `finish` (client cancel, network drop)
  may never reach the finish-time write, so usage is lost unless the tap
  accumulates incrementally; emit a `usage_kind=partial` row with whatever was
  observed (or none) rather than silently dropping the turn. **Do not apply that
  rule blindly to OpenAI/Codex compatibility streaming**: those handlers are
  compute-then-chunk, so the provider call and usage are complete before SSE
  emission. For those paths, write realized spend after a successful provider
  return and before/during synthetic chunk emission; client disconnect during
  chunk delivery is delivery telemetry, not partial provider spend. The split is
  by **handler, not by the word "OpenAI"**: the compatibility handlers in
  `openai_chat.c` (`chat_stream_handler` / `completion_stream_handler` /
  `responses_stream_handler`) are compute-then-chunk, whereas the OpenAI-*upstream*
  sub-path of `anthropic_http.c::messages_stream` feeds real provider SSE through
  the translator and **is** provider-incremental — so the partial rule applies
  there. Failed calls, if reported, are operational telemetry, not cost rows.
- **Source + model:** record source/client explicitly (Claude Code, Codex,
  webchat, OpenAI-compatible ingress, delegate) and resolve a real billable
  `model` (see §2a) instead of the empty string `agent_log_call` writes today.
  Webchat-proxied OpenAI-compatible requests need explicit forwarding metadata
  because the proxy intentionally removes the client `Authorization` header
  before the request reaches aimee-server.
- **Agent-log relationship:** for normal `agent_log_call` rows, write a shared
  call id into both `agent_log` and `token_audit` (or insert `agent_log` first and
  store its row id in `token_audit`). For direct ingress rows that intentionally
  have no `agent_log` row, leave that link empty and keep them out of agent-log
  aggregates. Never join the two tables by agent name/role for cost.
- **DB1 vs DB2:** keep the per-row ledger in DB1 (local, where `cmd_usage` and
  `insights.overview` already read). Add a DB2 mirror/aggregate **only** if a
  specific shared/multi-machine optimization-analytics need is established; if so,
  the proposal must state the DB1→DB2 relationship explicitly so operators never
  face two competing cost ledgers. Default: DB1 only.
- **Double-counting guard:** delegate child spend is already folded back to the
  parent via `db1_token_audit_cost_for_delegation()` and `db1_cost_fold_record()`.
  Ingress accounting must state whether parent summaries include child spend,
  exclude it, or show both with de-duplication.

### §2a Billable-model resolution (shared fix)

Define the precedence for the audited model and apply it to both ingress rows and
the existing `agent_log_call` empty-model weakness: **provider-reported model
(response) > resolved/served model (the primary agent aimee actually routed to) >
requested model**. Record the requested model separately when it differs (Claude
Code asks for one model; aimee serves the configured primary), so attribution is
auditable rather than silently wrong. Agent names are not pricing keys.

This also requires extending response/result shapes: `parsed_response_t` and
`agent_result_t` currently carry token counts but no provider-reported model and
no stop reason, so parsers must extract those fields before the ledger writer can
honor the precedence rule.

## §3 Cache-aware request shaping, scoped to real ingress ownership

Cache marking is valuable, but it must match the ingress and provider shape.
Anthropic caching is not a text transform — `cache_control` must land on the
correct structured system/content block in outbound provider JSON. OpenAI-style
prompt caching is automatic/provider-specific and usually has no portable
request metadata.

- **OpenAI/Codex ingress first:** target the live pre-injection seam in
  `src/server/openai_chat.c`. Because the current `<aimee-context>` is per-turn
  query-derived, do not assume the whole envelope is cacheable. Either split the
  pre-injection output into stable and volatile parts, or place cache boundaries
  on stable system/persona/history prefixes and leave volatile retrieval after
  the boundary.
- **Anthropic `/v1/messages` carve-out:** `anthropic_http.c` is explicitly a
  stateless proxy. Default behavior is to preserve client-supplied structured
  system blocks and any existing `cache_control` metadata. Adding Aimee-generated
  cache controls or context to this path requires a separate opt-in flag and
  tests proving it does not corrupt Claude Code-owned tools, messages, or system
  arrays.
- **Per provider shape:** Anthropic native `/v1/messages` — only emit
  `cache_control: {type:"ephemeral"}` on an owned structured block and only when
  the provider is Anthropic. OpenAI-compatible — no-op or provider-specific
  automatic caching; never emit Anthropic cache metadata. Translated
  OpenAI-via-Anthropic vs non-Anthropic driver paths must be handled explicitly.
- **The owned outbound `system` is a flat string today.** For the aimee-built
  path, `agent_build_request_anthropic` emits `cJSON_AddStringToObject(req,
  "system", system_prompt)` (`agent_bridge.c:197`) — a string, not a content-block
  array. So even on an aimee-owned request, placing `cache_control` on a stable
  block first requires restructuring `system` into a `[{type:"text", ...,
  cache_control:{...}}, ...]` array (Anthropic only), keeping the string form for
  every other provider. That conversion — not the flag — is the bulk of §3's
  Anthropic work, and it is why the OpenAI/Codex seam (no portable cache metadata,
  just prefix ordering) is the cheaper first target.
- **Negative tests required:** prove cache metadata is never leaked to an
  unsupported provider's request body; prove existing client-supplied
  `cache_control` blocks are preserved; prove Anthropic system arrays are not
  flattened when a structured mutation is needed.
- Savings are computed from realized `cache_read_tokens` (§2), not asserted.

## §4 Short-window dedup, narrowly scoped for v1

A response cache keyed only on `SHA256(body)` is unsafe. v1 is deliberately
narrow.

- **Key includes every behavior-affecting input:** provider/agent identity,
  resolved model, endpoint/provider, behavior-relevant config flags, the
  exact preinject/context identity, behavior-affecting request headers, auth
  principal or tenant boundary, and stream/non-stream mode.
- **Request-context dependency:** the idempotency key and account/source boundary
  are not available to `openai_chat.c` / `anthropic_http.c` today because handler
  signatures receive only the body. §2's request context must land before dedup,
  or dedup can only key on unsafe body-derived data. Webchat's OpenAI proxy must
  also preserve or restamp the idempotency key and source/account boundary when
  forwarding to aimee-server.
- **v1 eligibility:** buffered only, no tools / no server-side tools, non-stream,
  successful `200` only, explicit idempotency key, and no request surface that can
  consume changing memory/context unless that context hash is in the key. Anything
  that can emit tool calls, consume changing memory/context, or depend on
  time/session state is excluded unless replay semantics are proven.
- **Window:** small TTL (~5s), bounded map, with per-source/account isolation.
- **Savings:** record `usage_kind=avoided` with *avoided estimated cost*, not
  "full turn cost saved," unless the skipped provider call's price is
  deterministically known from a prior realized row. Avoided cost must not be
  added to spend totals.

## §5 Complexity score → reasoning-effort cap, mapped to real provider surfaces

A deterministic 0–10 complexity score (message count, content length, tool
presence) **caps** reasoning effort — it does not route (routing is the bandit,
§6).

- **Map to what exists:** aimee config exposes `model_reasoning_effort`, not a
  generalized numeric thinking budget. Anthropic native may use an
  extended-thinking budget object; OpenAI/Codex-compatible surfaces use a
  reasoning-effort enum; unsupported providers no-op.
- **Precedence:** an explicit user/provider setting is never overwritten by the
  cap unless a config flag opts into that. The cap only *lowers* effort on
  low-complexity turns (and may raise on tool-error signals).
- **Provider support:** request mutation must be driver/capability-aware. Do not
  send unknown reasoning fields to OpenAI-compatible providers that reject them.
- Lives in the request-shaping path; default-off.

## §6 Cost-shaped reward into the existing bandit (not raw dollars)

The `delegate_routing` close path takes a scalar reward in `[0,1]` over
beta-style posteriors. Feeding `cost_usd` directly would invert the semantics.

- **Reward shaping:** `reward = clamp01(quality_score − λ · normalized_cost)`,
  where `quality_score` is the existing success outcome (or a richer quality
  signal if available), `normalized_cost` is `cost_usd` normalized over a stated
  rolling window, and `λ` is a config weight. State the normalization window and
  the quality source explicitly.
- **Alternative (default first):** keep the reward scalar unchanged and store
  dollars as side metadata on the decision, consumed by `aimee optimize compare`
  for $-delta reporting without touching arm selection. Default to side-metadata
  first (lower risk), graduate to shaped reward behind a flag.
- **Accounting dependency:** shaped rewards must use realized child/delegate cost
  after cost-fold reconciliation, not an estimate based on agent name. If no
  realized cost exists, fall back to unchanged success reward and record why.

## §7 API surface: extend insights.overview before adding /v1/usage/*

`/v1/insights/overview` (`insights.overview`, implemented in
`src/server_insights.inc`) already reports usage/cost summaries. Default to
**extending it** with ingress/source breakdowns rather than adding `/v1/usage/*`.
If a separate route is still justified, the proposal must say why it does not
overlap `insights.overview` and keep the two non-overlapping.

Any new route requires:

- OpenAPI generator/source updates and regenerated route metadata;
- `server_http_routes.inc` registration that stays scanner-clean;
- CLI RPC route/client updates if the thin client exposes it;
- `server_auth.c` capability policy, not just route registration; and
- tests for auth denial as well as route/spec parity.

Extending an existing route still has contract work. If `insights.overview` is
the usage surface, `api/openapi-server-v1.yaml` must stop describing it as an
opaque `type: object` and document the realized/estimated/avoided/partial
breakdown, source/model arrays, and backwards-compatible legacy fields. After
that, regenerate `src/server/openapi_server_data.h` (`src/gen_openapi_server.py`)
so `/v1/openapi.yaml` serves the updated contract, and update
`src/cli_rpc_routes.inc::print_insights_overview` so the thin client displays the
new spend semantics instead of only the legacy `estimated_cost_usd` scalar.
Webchat is a **separate, undocumented** wire contract: the browser receives usage
as an SSE event carrying only `{in, out, cost}` (`webchat/socket.go`,
`webchat/chat.go`), so surfacing realized-vs-estimated to the browser means adding
a `usage_kind` (or a realized-cost field) to that SSE shape too — it is not
covered by the OpenAPI change.

Regardless of whether a new route is added, every existing consumer of
`token_audit` must learn the same semantics: `cmd_usage`, `insights.overview`,
HUD, dashboard JSON, `cmd_agent` / agent stats, the React context panel, and
webchat usage events must report realized spend separately from estimated,
avoided, and partial rows. A single SQL helper for spend totals is preferable to
copy-pasting `WHERE usage_kind = 'realized'` across consumers, and agent-log
metrics must use the new call key instead of the current agent-name join.

## §8 Design-review resolutions (roundtable 2026-06-16)

The 2026-06-16 design roundtable on the remaining §2–§7 work returned 12 blockers.
Each is resolved below; the decisions are normative for implementation. The single
biggest simplification: **v1 audit writes are SYNCHRONOUS** — one `token_audit`
insert on the handler thread after the provider returns. A local DB1 insert is
sub-millisecond against a multi-second LLM call, so it does not meaningfully load
the hot path, and synchronicity dissolves the entire async-queue blocker class
(B1) and the reward-barrier (B7). The `ingress_audit_async` queue is **deferred to
a later, separately-gated phase**, and may only ship with the durability spec in
B1.

**B1 — Async audit queue overflow/backpressure (§2).** v1 has no queue: writes are
synchronous (above). The deferred async phase, if ever justified by a measured
hot-path regression, MUST specify: a bounded queue (default 4096 rows); a
block-up-to-25ms-then-drop policy; a monotonic `ingress_audit_dropped_total` metric
surfaced to operators; and an explicit durability contract — "best-effort: rows are
dropped, never duplicated; drops are counted." Synchronous v1 is exactly-once
(idempotent insert, B4).

**B2 — Dedup confidentiality leak via shared webchat principal (§4).** The dedup
key MUST include the resolved principal/tenant id, and dedup is **fail-closed
disabled** for any request whose principal is shared/service (the anonymous
`webchat` service principal, or any principal flagged `shared`). Two logged-out
webchat users can therefore never be cross-served. v1 dedup is enabled ONLY for
requests carrying a non-shared, authenticated per-user identity. Test: a request
with a shared principal is never served from the dedup cache, even on identical
body.

**B3 — Backfill-from-`tool_name` misprices history (§2).** Do **not** backfill the
model from `tool_name` (agent names are not pricing keys, §2a). Legacy empty-model
rows are relabeled `"(unattributed)"` — safe, no false precision. Backfill an
actual model only for rows where a recorded served/provider model is independently
resolvable; otherwise leave unattributed.

**B4 — Idempotency attempt-id generator (§2).** The server-generated attempt id is
**128-bit random** (`platform_random`), lowercase-hex, namespaced by source. The
idempotent insert keys on `(source, request_id, attempt)`. Collision probability is
negligible at any realistic concurrency. On generator failure, fall back to the
in-process seen-set and skip the UNIQUE constraint for that row (accept a possible
*drop*, never a duplicate).

**B5 — Trusted-proxy credential reuse of `server.token` (§0/Config).** Do not reuse
the raw `server.token`. Use a **purpose-bound HMAC tag** —
`HMAC(server.token, "ingress-proxy-v1")` — so a compromised proxy credential does
not equal `server.token` compromise, and rotation of one does not silently break
the other. Document existing `server.token` consumers + the rotation procedure
before adding the proxy credential.

**B6 — `delegate_ensemble` pricing contradiction (§6).** Resolved as STALE: PR #185
routed `delegate_ensemble` through `token_estimate_cost` (unified pricing) — the §1
status is correct and the §6 precondition "`delegate_ensemble` bypasses unified
pricing" is removed. Cost-shaped reward reads realized delegate cost from
`token_audit` (populated via `agent_log_call`).

**B7 — Reward shaping vs write timing (§6).** Dissolved by synchronous writes (B1):
delegate/`agent_log_call` rows are written synchronously, so realized delegate cost
is available at `kb_client_bandit_close`. Cost-shaped mode additionally requires
that delegate-call audit rows are never routed through any future async queue (they
stay synchronous); if that ever changes, a flush barrier before bandit close is
mandatory. Target fallback (no-realized-cost) rate < 5%; above it, the feature
stays in side-metadata mode.

**B8 — Cache-shaping circular bootstrap (§3).** Marking is itself the measurement:
applying `cache_control` to a candidate stable prefix is **safe** — a prefix that
turns out volatile simply yields no cache hit, never a wrong answer. Bootstrap:
speculatively mark candidate prefixes (system/persona/history), measure realized
`cache_read` hit-rate over a rolling window (default 100 turns); **promote** to
"stable" (keep marking) when hit-rate ≥ 50%, **demote** (stop marking) when it
falls below 25% over the window. No pre-marking measurement is needed.

**B9 — Dedup key open-ended flag set (§4).** v1 keys on an **enumerated allowlist
only**: resolved model · provider/endpoint · `model_reasoning_effort` · temperature
(when set) · stream mode · principal/tenant (B2) · preinject/context hash. Any
handler-level, behavior-affecting config NOT in this allowlist **disables dedup for
that request** (fail-closed), rather than keying on a partial set. Test: introducing
an unlisted behavior flag must disable dedup, not silently collide.

**B10 — `sse_offload` context threading (§2).** Audit: `sse_offload` is used only by
the event-stream paths (`handle_run_events`, `handle_session_events`,
`handle_cli_session_stream`) — **none of the six no-log LLM handlers offload**; they
are synchronous compute-then-chunk on the request thread. So request context flows
via the scoped thread-local for all six; only the `/v1/runs` detached worker uses
capture-at-enqueue. A regression test asserts that any handler doing LLM spend on an
offloaded/detached path captures context at enqueue (fails if a new offload path
omits it).

**B11 — Thread-local context reset site (§2).** The context is set at **handler
entry** and cleared at **handler exit on all paths** via a scoped RAII-style helper
(`request_ctx_scope`), so an early-return or error cannot leak the prior request's
principal into the next request on a pooled worker thread. Reset is structurally
enforced by the helper, not by hand at each return. Test: a loop reusing one worker
thread across requests with distinct principals asserts no cross-request leak.

**B12 — Flag depends on net-new peer-cred capture (§2).** `ingress_usage_accounting_enabled`
gates on a runtime capability check: until `platform_ipc_peer_cred()` (UDS peer UID)
and the trusted-proxy mechanism (B5) are present, the flag **auto-degrades to
`source="unknown"`** rather than writing blank-source rows (or refuses to enable,
operator's choice). Source/model breakdowns are therefore never silently
meaningless.

**Design clarifications (the roundtable's non-blocking items), resolved:** partial
rows store the *observed* tokens with cost computed on those tokens and are excluded
from realized-spend totals (never pro-rated against an unknown "expected"); dedup
freezes the memory/context-hash version into the key and refuses to dedup across
version boundaries (B9 already excludes memory-consuming shapes from v1); reward
shaping defaults λ=0.3 with min-max `normalized_cost` over a 200-decision window and
ships only after an offline replay shows arm-selection isn't collapsed onto the
cheapest arm; the binary `quality_score` is renamed `success_indicator` (a real
quality signal is future work, not claimed now); `insights.overview` keeps the
opaque `overview` passthrough and adds typed fields under a new `usage_v2` key
(no client break); legacy agent-log rows without `call_id` stay on the old
agent/role join with a documented "historical estimate" caveat; the §5 complexity
cap ships only after an offline analysis bounds its false-low-complexity rate; and
avoided-cost binds the unit price to the current pricing authority at lookup time
(never caches a stale unit price across a pricing refresh).

**Delegate pricing is per-delegate configurable, default 0 (no external data
dependency).** Aimee's delegates are subscription-based, so their per-token price is
**0** — which is exactly what the static known-zero rows (#339) encode (priced=1,
"free", not "unknown", so the delegate economics never flat-rate them). There is no
missing price data to obtain. If a delegate ever becomes metered, its price is set
**on that delegate's model** via the single pricing authority — the model_registry
`cost_in_per_mtok` / `cost_out_per_mtok` fields (operator/models.dev override), which
`token_estimate_cost` already honours: a nonzero registry price overrides the static
0 (`token_tracker.c` → `token_tracker_registry.c` bridge → `model_capability_get`).
So pricing is configurable per delegate without touching code, and there is **no
second pricing source** (agents.json carries no price; price lives only in the
registry). This closes the previously-noted "price data" item — it was a framing
error, not a real dependency.

## Config (all default-off, flag-rollout-readiness program)

- `ingress_usage_accounting_enabled` (§2 — observe + audit ingress turns)
- `ingress_cache_marking_enabled` + `ingress_cache_min_chars` (§3)
- `anthropic_ingress_cache_mutation_enabled` (§3 carve-out; default off, likely
  later phase)
- `ingress_dedup_enabled` + `ingress_dedup_window_ms` (§4)
- `reasoning_effort_cap_enabled` (§5)
- `cost_reward_lambda` + `cost_reward_enabled` (§6; 0 / off ⇒ pure side-metadata)
- `ingress_audit_async` (§0/§2; write audit rows via an off-thread queue rather
  than synchronously in the streaming hot path)
- `trusted_ingress_proxy_token` or equivalent proxy credential (§0 findings
  18–19; required before forwarded source/principal headers are honored)

Most are scalar bool/int flags needing only a `config.h` struct field + a
`config_fields.c` row (+ a `test_config.c` round-trip). If the trusted-proxy
mechanism is a list or structured policy rather than one token, it also touches
`config_sections.c` / `config_save.c`. Do not rely on a bare UID list for
forwarded identity; peer UID is only the fallback principal when no proxy
credential is present.
The §1 pricing unification and the §2 model-write / `by_model` fixes are **not**
flagged — they are correctness fixes that land on by default.

Accounting (§2) is pure observation and is the first flag to flip; schema/report
changes should land before request mutation. The request-mutating optimizations
(§3–§5) and the shaped reward (§6) stay off until each clears the readiness bar
with a benchmark showing no quality regression.

## Testing

- **Pricing:** unknown-model → 0.0; cache-read/write fields applied; registry
  lookup/fallback; ambiguous substring ordering; provider alias normalization;
  **drift test** (token_tracker ≡ registry for every shared model);
  compound/adversarial id must not match an Anthropic price; zero-price (`minimax
  0.0/0.0`) is not silently flat-rated; `delegate_ensemble` cost uses the unified
  lookup.
- **Schema/reporting:** additive migration preserves existing `token_audit` rows;
  **by-model no longer hides legacy empty-model rows** (false-zero migration
  test); spend totals exclude `estimated`, `avoided`, and `partial` rows by
  default; source/model breakdowns are stable in `cmd_usage`,
  `insights.overview`, HUD, dashboard JSON, `cmd_agent` / agent stats, React
  context panel, and webchat usage events. Agent-log metrics do not multiply
  token_audit rows when multiple calls share the same agent/role.
- **OpenAI/Codex ingress audit (double-count guard):** each of the six
  synchronous no-log handlers (buffered+streaming chat/completions,
  buffered+streaming completions, buffered+streaming responses) writes **exactly
  one** realized row; **`/v1/runs` still writes exactly one** row (via its
  existing `agent_log_call`) and gains source attribution **without** a second
  write; `agent_execute()` remains no-log; normal `agent_run*` paths are
  unchanged. `/v1/embeddings` is **excluded from spend by default** (local $0
  embedder) and never appears as chat/completion model spend; if a remote embedder
  is wired it records under a distinct embedding usage-kind. Explicit assertion: no
  turn produces two rows.
- **Request context / threading:** registered handlers see request id, idempotency
  key, source/principal/session metadata, and route identity; the `/v1/runs`
  detached worker sees the **same** context via capture-at-enqueue (not a
  thread-local), and the thread-local resets per request so it never leaks to the
  next request on a reused worker thread. Webchat OpenAI proxy requests arrive at
  aimee-server with trusted source/session/principal metadata after
  `Authorization` is stripped. TCP clients and untrusted UDS peers cannot spoof
  forwarded source/principal headers, same-UID non-proxy UDS clients are not
  trusted merely because their UID matches webchat, and caller-supplied
  `X-Request-ID` is preserved only as correlation metadata, not as the sole
  idempotency key.
- **Source attribution:** ingress rows carry a per-client source derived from the
  request context (session key/principal/UDS peer UID), **not** the PPID-shared
  `session_id()`; two distinct clients with distinguishable identity do not
  collapse into one `top_sessions` row; a UDS request with no forwarding metadata
  still attributes to its peer UID rather than an anonymous blank source.
- **Webchat OpenAI shared-bearer clients (explicit non-goal for per-client
  attribution):** the webchat OpenAI proxy surface (`/v1/chat/completions`,
  `/v1/completions`, `/v1/responses`, `/v1/embeddings`) authenticates external
  callers with a single shared bearer, which by construction carries **no**
  trusted per-client identity. All stamped metadata is server-derived, never
  client-chosen: a request with a valid webchat session cookie is attributed to
  the trusted PAM username (`webchat:<pam-user>`) — so distinct logged-in users do
  **not** collapse — and a request with only the shared bearer is **intentionally
  attributed to the single trusted `webchat` service account** (principal
  `webchat`, no session key). The OpenAI `user` request-body field is deliberately
  ignored for attribution (a client must not be able to choose its own audit
  identity). The "two distinct clients do not collapse" criterion therefore
  applies to clients that present a distinguishable trusted identity — a webchat
  session today, or per-user OpenAI tokens if that surface later adds per-user
  auth — and NOT to anonymous callers sharing one credential, which are one
  account by definition.
- **Idempotency:** a retried or late-finalized call with the same
  `(source, request_id, attempt)` produces exactly one row (index or in-process
  guard), and the migration that creates the uniqueness constraint is exercised.
- **Aborted stream:** a stream cut before `finish` writes a `partial` row (or
  none) for provider-incremental streams, never a silently dropped turn. A
  compute-then-chunk OpenAI/Codex stream that completes the provider call writes a
  realized row even if the client disconnects during synthetic SSE delivery.
- **Buffered ingress:** provider usage writes **exactly one** audit row with
  resolved source + billable model; **no row** on provider failure.
- **Native Anthropic streaming:** final `message_delta.usage` is observed while
  the original stream is relayed byte-for-byte unchanged.
- **OpenAI-style streaming:** with `stream_options.include_usage` enabled only for
  supporting providers, full normalized usage (input + cache + output) is
  captured, not just `completion_tokens`; unsupported providers do not receive
  the option.
- **count_tokens:** asserts **no** spend ledger row.
- **Prompt-cache/request shaping:** OpenAI/Codex seam handles stable-vs-volatile
  preinject placement; the aimee-owned Anthropic `system` is emitted as a
  content-block array (not a flat string) with `cache_control` on the stable block
  only under the explicit opt-in carve-out; below-floor no-op;
  unsupported-provider no-op; no cache-metadata leakage; existing client cache
  controls preserved; non-Anthropic providers still receive a plain `system`
  string.
- **Dedup:** TTL; per-source/account/agent/model/endpoint/flags/context/stream
  key separation; no-tools/no-stream/200-only/idempotency-key eligibility;
  error-response bypass; no dedup when request context is unavailable; webchat
  proxy preserves or restamps idempotency/source metadata; avoided cost does not
  inflate spend totals.
- **Bandit reward:** reward stays in `[0,1]`; cost affects arm selection only
  through normalized shaping, never raw dollars; side-metadata mode leaves arm
  selection unchanged; missing realized cost falls back safely.
- **Model attribution:** ingress and `agent_log_call` rows resolve a non-empty
  billable model per §2a precedence; requested-vs-served divergence recorded.
- **Agent-log join:** normal agent calls write a shared `call_id` / `agent_log_id`
  into `agent_log` and `token_audit`; direct ingress rows without an `agent_log`
  row stay visible in source-aware usage summaries but do not contaminate
  `cmd_agent` agent stats. A fixture with two `agent_log` rows and two matching
  `token_audit` rows proves cost is not multiplied four ways.
- **DB1 write concurrency:** a load test with the new write sites firing from
  multiple request threads + the `/v1/runs` worker measures `token_audit` drop
  rate (busy-handler exhaustion) and tail latency; async mode keeps both within
  budget and the request thread is not blocked on the insert.
- **UDS/proxy trust boundary:** an untrusted UDS peer cannot set source/principal
  via forwarded headers — they are ignored; a same-UID but non-proxy UDS peer also
  cannot set them; a TCP client's `X-Forwarded-*`/principal headers are always
  ignored; only a peer with the proxy credential can forward identity; a UDS
  request with no forwarding still attributes to its captured peer UID.
- **OpenAPI/thin-client contract:** extending `insights.overview` updates
  `api/openapi-server-v1.yaml` with a concrete response schema, regenerates
  `src/server/openapi_server_data.h`, and updates `src/cli_rpc_routes.inc` so
  remote `aimee insights` displays realized spend separately from estimated,
  avoided, and partial rows.
- **API/auth:** any new usage route has route/spec parity, CLI RPC coverage where
  exposed, and `server_auth.c` capability tests. If only `insights.overview` is
  extended, test the existing route and capability.
- **Bench:** A/B on the bench corpus, §6 on vs off, reporting $/correct-answer;
  user-run job (no autonomous prod deploy).

## Non-goals

- No new pricing module, no new usage ledger, no new `aimee usage` command —
  unless `cmd_usage` and `insights.overview` are proven insufficient; extend
  `token_tracker` / model registry / `token_audit` / `insights.overview` /
  `cmd_usage` first.
- No SQLite `tracker.db`; per-row ledger stays in DB1, DB2 only if shared
  analytics is justified (§2).
- No silent message-trimming or model substitution; routing stays in the bandit.
- No mutation of Anthropic Messages ingress by default; it remains a stateless
  proxy unless a separate opt-in phase changes that contract.
- **aimee-kb-side LLM spend is out of scope.** Provider calls made inside the
  separate aimee-kb process (`/v1/rules/generate`, curator/deep-extraction) do not
  reach aimee-server's DB1 `token_audit`; this ledger is server-local. Folding kb
  cost in is a separate effort (a kb-side audit hook + cross-process report), and
  this proposal must not be read as covering it.
- No external proxy, menu-bar widget, standalone dashboard, or log-scraping of
  other tools' files — aimee owns the request path.
