# Proposal: ingress cost-accounting coverage + request-level cost optimizations

- **State:** draft - pending review (revised after PR #180 review + repeated
  file-by-file codebase audits)
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
  any required migration fields), `src/server/ingress_preinject.c` (envelope
  metadata/splitting only if needed by the OpenAI/Codex seam),
  `src/server_insights.inc` (the `insights.overview` implementation),
  `src/cmd_core.c` (`cmd_usage`), `src/server/server_compute.c` (cost-shaped
  reward on the existing `delegate_routing` bandit close), `src/server/server_http.c`
  + `src/headers/server_http.h` (request-context propagation for source,
  idempotency, principal, and request-id metadata), `webchat/openai.go` and
  `webchat/socket.go` (webchat proxy/session source attribution),
  dashboard/HUD/frontend readers that consume token-audit totals, config
  plumbing (`src/headers/config.h`,
  `src/config_fields.c`, `src/config_sections.c`, `src/config_save.c`), route and
  auth registration only if a new API method is still justified. DB2 only if a
  shared/multi-machine analytics need is
  established (§2). Unit + integration tests. No new service, no new model.

## Revision note

This supersedes the first draft, which proposed new `cost_pricing.{c,h}`, a new
db2 usage ledger, a new `aimee usage` CLI, and raw-dollar bandit rewards. The PR
#180 review correctly showed that aimee **already ships** cache-aware pricing
(`token_tracker.c`, `token_estimate_cost`), an audit ledger with much of the
needed schema (`db1/token_audit.c`: `session_id, delegation_id, model,
prompt/completion/cache tokens, estimated_cost_usd`), a reader (`cmd_usage`
→ `db1_token_audit_*`), a usage/cost summary route (`/v1/insights/overview`), and
model-registry price fields (`cost_in_per_mtok` / `cost_out_per_mtok`). All
verified in-tree. The objective is therefore **coverage and correctness of the
existing machinery for ingress requests**, not new accounting. Every §1–§6 below
is rewritten accordingly.

This revision also folds in second-pass review findings: Anthropic
`/v1/messages` is intentionally a stateless proxy and is **not** a current
pre-injection surface; the shipped pre-injection seam is in `openai_chat.c`; the
`<aimee-context>` envelope is per-turn query-derived and therefore not
inherently a stable cache prefix; and the current `token_audit` schema lacks
several fields this proposal needs (`source`, `usage_kind`, requested-vs-served
model, duration/stop metadata, optimization metadata). Those are explicit work
items below rather than assumptions.

A third pass added a **file-by-file codebase audit** that confirmed the above and
surfaced four issues no review named, now folded in with file:line evidence:
(1) pricing lives in **four** divergent sources — token_tracker (substring +
cache), the model registry (exact, no cache), `delegate_ensemble.c` (a third
flat-rate calculator), and `models_dev_snapshot.json` — so the same model id can
be priced differently by different paths (§0/§1); (2) `db1_token_audit_by_model`
filters out every empty-model row, so the model-write fix and the filter fix must
land together or history shows a false zero (§0/§2); (3) the aimee-owned outbound
Anthropic `system` is a **flat string**, so the cache carve-out's real work is a
string→content-block conversion, not a flag (§3); (4) `stream_options.include_usage`
is set nowhere and aborted streams drop usage (§2).

This pass adds three remaining blockers: (5) OpenAI/Codex ingress does **not**
go through `agent_log_call` either — `openai_chat.c` calls `agent_execute()` and
`agent_execute_messages()` directly, and `agent_execute()` explicitly leaves
logging to its callers — so those handlers need first-class audit writes, not
only attribution fixes; (6) the registered HTTP handler signatures only receive
`body`/`resp`/`cap`, so source/principal/idempotency/request-id metadata is
unavailable unless `server_http.c` exposes a request context; (7) HUD, dashboard,
frontend, and webchat consumers currently sum `estimated_cost_usd` as a scalar,
so new `estimated` / `avoided` / `partial` rows will inflate spend unless every
reader is updated with the same semantics.

A fifth pass (four more parallel audits) verified 5–7 and corrected/added five
points: (8) **`/v1/runs` already writes a row** (detached worker →
`agent_run_with_tools` → `agent_log_call`), so it must get attribution **only**,
not a new write — adding one double-counts; the new writes belong only to the six
synchronous no-log handlers; (9) a **thread-local context is insufficient** —
the `/v1/runs` worker is a detached pthread that does not inherit request-thread
TLS, so its context must be captured into the job struct at enqueue; (10)
**`session_id()` is PPID-process-wide**, so all ingress rows collapse into one
session and per-source attribution must come from the request context, not
`session_id()`; (11) **idempotency contradicts the additive reconcile** — a
UNIQUE index is required and the reconcile creates none (and `NOT NULL` columns
need defaults), so an explicit migration or in-process idempotency is needed; (12)
the scalar-spend reader set is **eight** files incl. `server_compute.c:1469`
cost-fold and `webchat/socket.go`, but `Chat.tsx` does not price client-side so
§1 propagates to the UI for free.

A sixth pass checked the webchat OpenAI proxy and embedding ingress: (13)
`webchat/openai.go` authenticates OpenAI-compatible clients at the webchat edge,
then `proxyV1` strips `Authorization` and forwards over the aimee-server UDS
without adding replacement source/session/principal context, so server-side
request-context accounting will see webchat-originated OpenAI traffic as generic
trusted local traffic unless the proxy stamps explicit forwarding metadata; (14)
`/v1/embeddings` is an OpenAI-compatible ingress route proxied by webchat, but
`embeddings_handler` calls `memory_embed_text` and returns OpenAI-style
`usage.prompt_tokens` without using the LLM audit path, so embeddings need an
explicit non-LLM / estimate-only / separate embedding-spend classification rather
than being folded into chat/completion cost.

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
- **Webchat's OpenAI proxy erases source/principal context unless it stamps
  replacement metadata.** `webchat/openai.go` validates the caller's webchat
  OpenAI bearer token, then `proxyV1` removes `Authorization` before forwarding
  `/v1/chat/completions`, `/v1/completions`, `/v1/responses`, and
  `/v1/embeddings` over the UDS. If the server context layer only inspects the
  forwarded HTTP request, these calls look like anonymous local traffic instead
  of webchat traffic tied to a session/user. The proxy must add trusted internal
  forwarding headers or an equivalent side channel for source, principal/session,
  original request id, and idempotency.
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
- **Existing usage readers assume every row is spend — full list.** The audit
  found **eight** consumers that sum `estimated_cost_usd` (or token-audit totals)
  with no `usage_kind` filter: `src/hud.c`, `src/cmd_core.c` (`cmd_usage`),
  `src/dashboard.c`, `src/server/dashboard_server.c`, `src/server_insights.inc`,
  `src/server/server_compute.c:1469` (the cost-fold query),
  `webchat/socket.go` (`streamEvent.Cost`), and `frontend/src/pages/Chat.tsx`.
  Once `usage_kind=estimated|avoided|partial` exists, all of them (plus the eight
  SQL aggregations in `token_audit.c`) must split realized spend from
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
  `stop_reason`, and `optimizations_json` / `avoided_cost_usd` if dedup/cache
  savings are reported. If the decision is to avoid migration, state exactly how
  each field is represented and which reports lose fidelity. Two migration
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
- **Request context prerequisite:** before adding source-aware audit rows or
  dedup, expose a per-request context from `server_http.c` to registered handlers:
  method/path, generated `X-Request-ID`, inbound idempotency key, session key,
  remote/local transport, bearer scope/principal (or local UID), and selected
  route capability. The thread-local preinject override is a precedent for the
  **synchronous** handlers only; because the `/v1/runs` worker is a detached
  pthread (and SSE may offload), the context for those paths must be **captured
  into the job/work struct at enqueue**, and the thread-local must be reset per
  request so it never leaks across reused worker threads.
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
- **Embeddings ingress is not chat/completion spend.** `/v1/embeddings` returns
  OpenAI-style usage, but the implementation is embedder-backed rather than a
  chat/completion provider turn. Either exclude it from spend dashboards, record
  it as estimate-only local usage, or add an explicit embedding usage kind and
  embedder pricing source if external embedding billing is possible. Do not let
  embedding prompt tokens inflate LLM model cost.
- **Buffered path:** parse the provider JSON `usage` block after a 200 response
  and before translating it back to the client. The Anthropic parser already
  extracts cache tokens (`agent_bridge.c:791-796`), but the **OpenAI buffered
  parser drops them** — this is an *add extraction* task, not just "verify they
  survive normalization."
- **`count_tokens`:** never writes a realized spend row. If exposed in usage
  summaries, it must be `usage_kind=estimated` and excluded from spend totals by
  default.
- **Failure / aborted streams:** no realized-spend row on provider error. A
  stream cut before `finish` (client cancel, network drop) never reaches the
  finish-time write, so usage is lost unless the tap accumulates incrementally;
  emit a `usage_kind=partial` row with whatever was observed (or none) rather
  than silently dropping the turn. Failed calls, if reported, are operational
  telemetry, not cost rows.
- **Source + model:** record source/client explicitly (Claude Code, Codex,
  webchat, OpenAI-compatible ingress, delegate) and resolve a real billable
  `model` (see §2a) instead of the empty string `agent_log_call` writes today.
  Webchat-proxied OpenAI-compatible requests need explicit forwarding metadata
  because the proxy intentionally removes the client `Authorization` header
  before the request reaches aimee-server.
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

Regardless of whether a new route is added, every existing consumer of
`token_audit` must learn the same semantics: `cmd_usage`, `insights.overview`,
HUD, dashboard JSON, the React context panel, and webchat usage events must report
realized spend separately from estimated, avoided, and partial rows. A single SQL
helper for spend totals is preferable to copy-pasting `WHERE usage_kind =
'realized'` across consumers.

## Config (all default-off, flag-rollout-readiness program)

- `ingress_usage_accounting_enabled` (§2 — observe + audit ingress turns)
- `ingress_cache_marking_enabled` + `ingress_cache_min_chars` (§3)
- `anthropic_ingress_cache_mutation_enabled` (§3 carve-out; default off, likely
  later phase)
- `ingress_dedup_enabled` + `ingress_dedup_window_ms` (§4)
- `reasoning_effort_cap_enabled` (§5)
- `cost_reward_lambda` + `cost_reward_enabled` (§6; 0 / off ⇒ pure side-metadata)

These are all scalar bool/int flags, so each needs only a `config.h` struct field
+ a `config_fields.c` row (+ a `test_config.c` round-trip); `config_sections.c` /
`config_save.c` are touched only if a flag is nested under a new compound object.
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
  `insights.overview`, HUD, dashboard JSON, React context panel, and webchat
  usage events.
- **OpenAI/Codex ingress audit (double-count guard):** each of the six
  synchronous no-log handlers (buffered+streaming chat/completions,
  buffered+streaming completions, buffered+streaming responses) writes **exactly
  one** realized row; **`/v1/runs` still writes exactly one** row (via its
  existing `agent_log_call`) and gains source attribution **without** a second
  write; `agent_execute()` remains no-log; normal `agent_run*` paths are
  unchanged. `/v1/embeddings` is either excluded from LLM spend or recorded under
  an explicit embedding usage kind, and never appears as chat/completion model
  spend. Explicit assertion: no turn produces two rows.
- **Request context / threading:** registered handlers see request id, idempotency
  key, source/principal/session metadata, and route identity; the `/v1/runs`
  detached worker sees the **same** context via capture-at-enqueue (not a
  thread-local), and the thread-local resets per request so it never leaks to the
  next request on a reused worker thread. Webchat OpenAI proxy requests arrive at
  aimee-server with trusted source/session/principal metadata after
  `Authorization` is stripped.
- **Source attribution:** ingress rows carry a per-client source derived from the
  request context (session key/principal), **not** the PPID-shared `session_id()`;
  two distinct clients do not collapse into one `top_sessions` row.
- **Idempotency:** a retried or late-finalized call with the same
  `(source, request_id, attempt)` produces exactly one row (index or in-process
  guard), and the migration that creates the uniqueness constraint is exercised.
- **Aborted stream:** a stream cut before `finish` writes a `partial` row (or
  none), never a silently dropped turn.
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
- No external proxy, menu-bar widget, standalone dashboard, or log-scraping of
  other tools' files — aimee owns the request path.
