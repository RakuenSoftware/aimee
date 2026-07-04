# Proposal: Gateway mutation — primary-agent context reduction

- **State:** done — implemented + merged to `testing` behind `reduce_gateway_mutate`
  (default OFF, zero behavior change when off), across 7 roundtable-reviewed PRs
  (#1015 config, #1017 session-breaker+telemetry, #1018 decision helpers, #1019
  Anthropic buffered, #1021 Anthropic streaming, #1022 OpenAI `/v1/responses` buffered,
  #1023 telemetry+docs). See **Close-out** below for the acceptance mapping, the
  runtime `.253` validation, the OpenAI-streaming finding, and the carried gates.
  Extends the unified context economizer (`context_reduce`/`context_fold`/`compact_body`,
  default-ON at the delegate seam). Design roundtable-reviewed (3 rounds; all decisions
  D1–D6 + the `should_apply`/snapshot/hard-bypass refinements incorporated below).
- **Thesis:** the economizer reduces the **delegate** (sub-agent) turn loop, but the
  inbound `/v1` **gateway** — the path that serves the **primary** agent (including
  Claude-Code-through-aimee) — runs the economizer in **shadow** (`measure_only=1`
  hardcoded: `anthropic_http.c:254`, `openai_chat.c:906`). It measures the primary
  agent's reduction opportunity and records a forecast, but never applies it, so the
  primary agent's tokens are not reduced. This proposal makes the gateway **apply** the
  reduced messages to the live request — under a default-off flag, compress-only,
  behind a per-session circuit breaker — so the primary agent reduces too.

## Charter roles
Reduce (the context economizer) extended to the **inbound** request-assembly seam. Reuses
`context_reduce` unchanged, the per-format boundary invariants, `message_history_repair`,
and the hard-bypass contract. No new reduction algorithm.

## Goal
Primary-agent context reduction that **cannot *persistently* break live client traffic
without circuit-breaking subsequent turns**: a reduced request the provider rejects is
caught (buffered: retried from the pristine original; streaming: the offending session is
disabled for **subsequent** turns — the current stream completes as the upstream
delivered it, per §7 R1), the working set is never elided, and the whole feature is
default-off behind a flag with numeric `.254` validation gates before any default-on
decision.

## §0 Why this is higher-risk than the delegate seam
The delegate seam mutates a sub-agent's request — a contained blast radius. The gateway
mutates the **live client-serving** request (`/v1/messages`, `/v1/responses`). A reduction
bug there surfaces to the end user. The design is therefore correctness-first and
default-off, and treats streaming and buffered paths separately because their recovery
options differ fundamentally (§2).

## §1 Precondition (verified by recon)
- **Buffered (`stream=0`):** the upstream HTTP status is known *before* any bytes are
  sent to the client (`anthropic_http.c:424`; `openai_chat.c:981`). A 400-retry-from-
  pristine is feasible.
- **Streaming (`stream=1`):** `write_sse_headers()` commits HTTP 200 to the client
  (`server_http.c:1188`/`:1178`) *before* the upstream status is known. Once streaming
  starts, a mid-stream retry-from-pristine is **architecturally impossible**.
- No existing gateway 400-retry (`http_retry_post_context` deliberately does not retry
  4xx). Inbound headers are readable via `http_header()` (`server_http.c:1354`).
- The provenance marker `reduce_state.reduced` already travels in-payload, so a request
  crossing both the gateway and the delegate seam is not double-reduced.

## §2 Design

### §2.1 Decisions (roundtable D1–D6)
- **D1 — Streaming mutates** under reduction-correctness guarantees + per-session disable;
  **no mid-stream retry.** This is the same contract the delegate seam already operates
  under (it mutates with no 400-from-pristine either — only hard-bypass + repair).
- **D2 — New flag `reduce_gateway_mutate` (default OFF)**, separate from the existing
  shadow `reduce_gateway_seam`. `mutate=1` implies `seam=1` (auto-enable + a single
  startup WARN), so the shadow baseline the validation gates need always exists.
- **D3 — Compress-only at the gateway in v1.** Fold is deferred to a follow-up
  (`reduce_gateway_fold`) — its boundary edge-cases concentrate the mutation risk.
  Compress-only captures ~30–50% of the saving with the smallest blast radius.
- **D4 — Per-session disable** (in-process LRU, §2.4).
- **D5 — No inbound header opt-in in v1** (`X-Aimee-Reduce` may be added later as an
  additive filter).
- **D6 — Provenance reuse** (`reduce_state.reduced`) is sufficient across the seam
  boundary, subject to a CI verification gate (§2.6); **clear-on-restore is mandatory**.

### §2.2 The mutation gate — `should_apply()` + hard-bypass
`should_apply(reduced, pristine)` returns true **only if**: the reduce reported no internal
error; the result is a genuine net shrink (a no-op reduce is not a mutation); a
`message_history_repair` run on the reduced result reports no structural violation (no
tool_use/tool_result split); and the replace installs cleanly. Otherwise the site
**hard-bypasses** — forwards the pristine request, records the reason, and skips the
restore path. Hard-bypass is not itself a disable trigger.

The reduce **internal-error** enum is explicit and is the same set surfaced in
`gateway_hard_bypass{reason}` and the §5 step-11 CI fixtures: `reduce_alloc_failed`,
`reduce_parse_failed`, `reduce_internal_assertion`, `reduce_format_unsupported`. The
non-error hard-bypass reasons are `no_op`, `structural_violation`, `snapshot_oom`, and
`replace_failed` (where `replace_messages_for_upstream()` collapses allocation failure,
unexpected-NULL content, size mismatch, and encoder error into the single `replace_failed`
tag).

**Construction-failure invariant (blocking).** The "never send an un-restorable reduced
payload" rule extends *past* `should_apply`: **if any step after `should_apply` returns
true fails before the upstream request is fully constructed and dispatched — OOM during
header/body serialization, JSON-encode failure of the reduced body, transport setup error,
any construction-time error — the gateway MUST fall back to the pristine snapshot and abort
the mutation** (reason `construct_failed`). This is a §5 step-11 fixture that injects a
failure between `should_apply` and dispatch and asserts pristine was sent.

**Provenance ordering (blocking-adjacent).** `reduce_state.reduced` is set on the live
payload **only after replace succeeds**, and is cleared on every hard-bypass path
(`replace_failed`, `construct_failed`, `snapshot_oom`, `structural_violation`,
`internal_error`, `no_op`). A partial-replace failure therefore never leaves a falsely
`reduced=true` payload that the delegate seam would skip.

### §2.3 Snapshot semantics
`snapshot_messages()` is a **deep copy** in v1 (every message + content string independently
allocated), so restoring pristine is independent of any retained references (reply framing,
provider response objects). On allocation failure it returns NULL → the site hard-bypasses
(**never send a reduced payload that cannot be restored**); **all partially-copied
sub-objects are freed before returning NULL** (no leak on partial failure). Refcount/COW is
a post-v1 optimization, gated on the §9 O2 benchmark.

On the **streaming** path the snapshot exists *solely* to enforce the un-restorable
invariant (it is held as a reference and freed after the reduce/replace decision; it is
**never restored** — streaming recovery is disable-only). Its latency/memory cost is
included in the §9 O2 benchmark, not just the buffered path.

### §2.4 Per-session disable (D4)
New module `msg_session_disable.{c,h}`:
- **Mutation requires a resolvable per-identity session key.** The key is derived (ordered)
  from: an inbound `aimee-session-id` header **validated against the auth identity**, else
  `SHA-256(bearer_token)[16hex]`. **A request with neither a validated header nor a bearer
  is NOT mutated** — it is a pristine passthrough, and **no disable state is written for
  it.** This removes the process-global `_anonymous` bucket entirely, so one caller's
  failure can never disable reduction for an unrelated caller (the §7 R5 cross-caller
  denial-of-feature hazard).
- **Header format + validation:** the `aimee-session-id` value MUST equal
  `SHA-256(auth_identity)[16hex]` (lowercase hex, exactly 16 chars). Any other value —
  mismatch, wrong length, non-hex, control chars — is treated as **absent** (fall through
  to the bearer hash), with at most one WARN per 60 s per source IP. An attacker holding a
  valid bearer therefore cannot forge another identity's session key.
- **Storage:** bounded LRU (cap 10k, ~1 MB), value `{disabled, expires_at, reason}`. **Sweep
  cadence:** on insert when size exceeds cap/2, plus a coarse wall-clock sweep every 60 s.
- **TTL:** `reduce_gateway_session_disable_ttl_ms` (default 1 h). **Must be > 0** — `0` and
  negative values are **rejected at config validation as startup-fatal** (a permanent-pin or
  breaker-off knob on a live path is disproportionate runtime risk; the diagnostic
  breaker-off mode, if ever needed, is a debug-only build flag, not a runtime knob).
- **Scope:** process-local; no cross-process replication in v1.

### §2.5 Path-by-path
**Buffered (both providers):** `snapshot → reduce → should_apply → send`. On the upstream
response of a **mutated** request:
- **Any 4xx** (not just 400 — a reduction can alter serialization into 413/422/etc.):
  restore pristine + `message_history_repair` (idempotent — the pristine is unmodified;
  defense-in-depth only, no signal of pristine corruption) + **resend once** + disable the
  session + clear `reduce_state.reduced`. Counter: `gateway_4xx_restore_resend`.
- **5xx:** **disable the session only — do NOT resend** (provider state is uncertain after a
  5xx; retrying from pristine is too speculative). Counter: `gateway_5xx_disable`. The 5xx is
  forwarded to the client as the provider returned it; it is never silently un-classified.

OpenAI adds a **new** `gateway_retry_post_with_reduction()` wrapper — it does **not** modify
`http_retry_post_context` (whose 4xx semantics must not change).

**Streaming (both providers):** `snapshot (un-restorable-invariant only) → reduce →
should_apply → open stream`. Failure detection is at the **SSE decoder layer, not a raw
read loop**: events are reassembled across TCP boundaries (a split frame is held in a
single-event classification buffer — no full-response buffering, no rewrite of
client-visible bytes), then forwarded as-is. Disposition of a **mutated** stream:
- **Invalid-request-class error frame** (Anthropic `error.type` ∈ invalid_request /
  embedded 4xx status; OpenAI `error.code` invalid-request) → disable the session for
  **subsequent** turns. These correlate with a bad reduced payload.
- **Unparseable / malformed frame, decoder-state error, or a non-SSE upstream failure after
  the 200 commit** (TCP reset, mid-stream 5xx body) → **fail-safe: disable the session** and
  forward whatever arrived; the current turn is already lost (200 committed).
- **Rate-limit / content-filter / server-error frames** → forwarded **without** disabling
  (they are not reduction bugs and must not false-positive the breaker).

In all streaming cases the current turn completes as the upstream delivered it; only
**subsequent** turns in the session are affected.

### §2.6 Provenance across the seam (D6)
| Scenario | `reduced` after gateway | Delegate seam | Correct? |
|---|---|---|---|
| Gateway reduced | `true` | skips | ✅ |
| Gateway hard-bypassed | `false` | attempts | ✅ |
| Gateway reduced → 400 → restored | **cleared** | attempts on the restored original | ✅ |
| Session disabled | `false` | attempts | ✅ |

CI verifies clear-on-restore, clear-on-hard-bypass, clear-on-OOM, and the
gateway→delegate hand-off in both directions. If the in-payload marker does not survive
the actual boundary in code, add an internal portable marker — verification is the gate,
not assumption.

## §3 Configuration
```
reduce_gateway_seam                    = 1        # existing — shadow/measure
reduce_gateway_mutate                  = 0        # NEW — apply reduction; default OFF
reduce_gateway_session_disable_ttl_ms  = 3600000  # NEW — 1 hour
```
`mutate=1` while `seam=0` is inconsistent → auto-enable seam **in memory only** (never
modifies the config file) + one startup WARN; a config left at `seam=0, mutate=1` repeats
the WARN each start until corrected. `reduce_gateway_session_disable_ttl_ms` **must be > 0**
(0 / negative are startup-fatal config errors, per §2.4).

## §4 Telemetry (the validation input)
Counters/histogram on the gateway path: `gateway_mutate_attempted`, `…_applied`,
`gateway_hard_bypass{reason}` (reasons per §2.2), `gateway_4xx_restore_resend`,
`gateway_5xx_disable`, `gateway_stream_error_disable` (the streaming invalid-request +
fail-safe disables), `gateway_session_disabled_set{reason}`,
`gateway_session_disabled_blocks`, `gateway_token_delta_pre_post_sampled` (≤1% sample). The
shadow baseline is produced **in parallel on every mutated request**: `measure_only` still
runs on the pristine snapshot and forecasts what the shadow path would have done, so the
mutated-vs-shadow comparison (§6) is on identical payloads, not a separate traffic window.

## §5 Implementation plan
Steps 1–5 + 10 ship behind `reduce_gateway_mutate=0` with **zero behavior change**; step 4
is pure refactor. Steps 6–9 are the wiring; step 11 is the gate to `.254`.

1. Config flags + startup consistency check.
2. `msg_session_disable.{c,h}` (LRU, TTL, sweep).
3. `session_id_resolve()` with header-vs-auth validation.
4. Extract `snapshot_messages()` (OOM-aware), `snapshot_token_count()`,
   `replace_messages_for_upstream()` helpers.
5. `should_apply()` + `hard_bypass()`.
6. Anthropic buffered: mutate + restore + resend + disable + clear-provenance.
7. Anthropic streaming: mutate + SSE error-frame inspect-as-forward + disable.
8. OpenAI buffered: `gateway_retry_post_with_reduction()` wrapper.
9. OpenAI streaming: symmetric to Anthropic (separate review — provider framing differs).
10. Telemetry.
11. CI: snapshot/restore equivalence, OOM hard-bypass, repair idempotency, provenance
    across the seam, disable-map TTL/sweep, header-vs-auth validation, SSE error-frame
    detection.

## §6 Validation gates (`.254`, before any default-on)
≥7 days with `reduce_gateway_mutate=1`, ≥10k mutated requests per provider per stream mode:

| Metric | Pass | On failure |
|---|---|---|
| Upstream 4xx, mutated vs shadow (same payloads) | within ±0.5% abs | reduce levers / roll back |
| `gateway_5xx_disable` rate, mutated vs shadow | within ±0.5% abs | investigate / roll back |
| `gateway_session_disabled_set` rate | < 5% of mutated sessions | reduce levers / roll back |
| `gateway_hard_bypass` rate | < 1% of attempts | investigate reduce error paths |
| `gateway_4xx_restore_resend` rate | < 2% of mutated requests | investigate boundary edges |
| `gateway_stream_error_disable` rate | < 2% of mutated streams | investigate streaming detection |
| user-visible stream-abort (after first byte, before `[DONE]`) | **zero** | hard rollback |
| Pre→post token delta (sampled) | monotone reduction (never +5% vs shadow) | investigate regression |
| Buffered p99 latency | < +10 ms vs shadow | benchmark snapshot cost |

**Hard rollback:** any user-visible failure not contained by the per-session disable (incl.
a non-disabling stream abort, or a 4xx/5xx that doesn't trip the breaker) → flip
`reduce_gateway_mutate=0` pending investigation.

## §7 Risks
- **R1 — Streaming without retry is new ground.** Per-session disable is the only runtime
  circuit breaker in v1; a global rolling-window flood-disable is designed and ready
  (§9 O1) but deferred — reconsider after the first week of `.254` data.
- **R2 — Snapshot cost on large contexts.** Deep copy at 1 M tokens is the open benchmark
  (§9 O2); buffered p99 is a §6 gate. COW is a post-v1 optimization.
- **R3 — `message_history_repair` after restore** — defense in depth; idempotency
  CI-verified.
- **R4 — OpenAI streaming framing parity** — separate review; the only provider-specific
  catch in v1.
- **R5 — Session-id spoofing + cross-caller denial-of-feature** — header-vs-auth validation
  is mandatory; mutation requires a resolvable per-identity key and writes **no** disable
  state for identity-less requests (no shared `_anonymous` bucket, §2.4), so one caller can
  never disable another. Security review before `.254`.
- **R6 — Default-ON** is a separate decision, out of scope for this proposal.

## §8 Non-goals
Mid-stream/SSE retry (architecturally blocked); recovery of the current streaming turn
(only subsequent turns are protected); mutation of identity-less requests (pristine
passthrough, no disable state); cross-process session-disable replication; new reduction
algorithms; inbound-header opt-in; the fold lever on the gateway path; default-ON;
refcount/COW snapshots (correctness-first); the global flood-disable (§9 O1).

## §9 Open items
- **O1 — Global rolling-window flood-disable** (second-order circuit breaker): a fixed
  256-entry process-local ring buffer; if ≥ N disable events in M seconds, send everything
  pristine until the window drains. Defaults N=5/M=300 s. ~50 LOC. **Deferred**; ratify
  only if a model flap is a concern during validation.
- **O2 — Snapshot benchmark** (200k/500k/1M tokens, deep-copy vs COW). Owned by perf;
  must report before `.254`. Pass: p99 < +15 ms at 1 M tokens.

## §10 Acceptance
1. `reduce_gateway_mutate=0` → byte-identical to today (shadow if `seam=1`); proven by a
   no-behavior-change test.
2. Buffered mutate + 400-restore-resend + session-disable + clear-provenance, both providers.
3. Streaming mutate + SSE error-frame disable, both providers; no mid-stream retry.
4. `should_apply()` hard-bypasses on every failure class (incl. snapshot OOM) and never
   sends an un-restorable reduced payload.
5. Provenance CI gate (§2.6) green.
6. The full economizer CI matrix + the new gateway tests green; default-OFF.
7. `.254` validation (§6) met before any default-on proposal.

```yaml acceptance
- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_msg_session_disable (LRU cap/eviction, sweep on insert>cap/2 + 60s wall-clock, TTL>0-required rejects 0/negative at config-validate; session-key = validated aimee-session-id (==SHA256(auth)[16hex]) -> hashed-bearer; identity-less request writes NO disable state; malformed/wrong-length/non-hex header treated as absent; cross-tenant: attacker bearer A + forged session-id B cannot disable B)"}
- {id: 2, tier: mechanical, check: "make unit-tests TEST=test_gateway_mutate (should_apply: net-shrink + message_history_repair-clean + replace-ok; hard-bypass reasons reduce_alloc_failed/parse_failed/internal_assertion/format_unsupported/no_op/structural_violation/snapshot_oom/replace_failed/construct_failed each forward pristine; CONSTRUCTION-FAILURE fixture injects failure between should_apply=true and dispatch -> pristine sent; snapshot deep-copy independence + partial-alloc frees before NULL + OOM-never-sends-un-restorable; provenance set ONLY after replace succeeds + cleared on every hard-bypass/restore path)"}
- {id: 3, tier: mechanical, check: "make build-integrity && make docs-gen-check && make schema-sync-check (reduce_gateway_mutate + reduce_gateway_session_disable_ttl_ms plumbed; default-off round-trips; mutate=1+seam=0 auto-enables seam in-memory + WARN; ttl<=0 startup-fatal)"}
- {id: 4, tier: integration, check: "with reduce_gateway_mutate=0 the inbound /v1 request is byte-identical to today (shadow if seam=1) — no-behavior-change test for both providers, buffered + streaming"}
- {id: 5, tier: integration, check: "buffered Anthropic + OpenAI: a mutated request that returns ANY upstream 4xx (400/413/422) restores the pristine original, resends once, disables the session, clears reduce_state.reduced (delegate seam re-attempts on the restored original); a mutated 5xx disables the session WITHOUT resend (gateway_5xx_disable) and forwards the 5xx"}
- {id: 6, tier: integration, check: "streaming Anthropic + OpenAI: SSE decoder-layer detection with a SPLIT-FRAME fixture (error event straddling a TCP boundary is reassembled + caught); invalid-request error frame disables subsequent turns; unparseable frame / non-SSE post-200 failure (TCP reset) fail-safe disables; rate-limit/5xx error frames forwarded WITHOUT disabling; no full-response buffering, no client-byte rewrite, no mid-stream retry"}
- {id: 7, tier: deployment, check: ".254 with reduce_gateway_mutate=1 over >=7 days, >=10k mutated requests/provider/stream-mode: upstream 4xx within +/-0.5% abs vs parallel shadow on same payloads, gateway_5xx_disable within +/-0.5%, session-disable <5%, hard_bypass <1%, 4xx-restore-resend <2%, stream_error_disable <2%, ZERO user-visible stream-aborts, sampled token delta monotone-reduction, buffered p99 <+10ms vs shadow"}
- {id: 8, tier: hardware, check: "snapshot_messages() bench at 200k/500k/1M tokens (deep-copy vs COW), both providers AND both buffered + streaming snapshot paths: p99 <+15ms at 1M tokens vs no-snapshot baseline on target hardware (O2, before .254)"}
```

## Close-out

Implemented as 8 planned slices (S7 subsumed — see below), each roundtable-reviewed and
merged to `testing` behind `reduce_gateway_mutate` (default OFF). Operator guide:
`docs/features/economizer-gateway-mutation.md`.

**Slice → PR:** S1 config flags + startup-fatal ttl + mutate⇒seam auto-enable (#1015);
S2 `msg_session_disable` breaker + `gw_mutate_stats` sink (#1017); S3 `gateway_mutate`
decision/snapshot/replace/provenance helpers + additive `reduce_error_t` (#1018);
S4 Anthropic buffered wiring + `gateway_mutate_wire` + identity capture (#1019);
S5 Anthropic streaming (SSE decoder-layer inspect-as-forward + disable) (#1021);
S6 OpenAI `/v1/responses` buffered via reference-boxing (#1022); S8 sampled token-delta
telemetry + no-behavior-change test + docs (#1023).

**Inbound endpoint coverage** (the gateway-mutation seam extends the existing economizer
*shadow* seam, which lives only on the two primary-agent endpoints):

| Endpoint | Handler | Upstream | Mutation |
|---|---|---|---|
| `/v1/messages` buffered | `anthropic_http.c messages_buffered` | buffered | S4 (restore-resend / 5xx-disable) |
| `/v1/messages` streaming | `anthropic_http.c messages_stream` | true SSE stream | S5 (disable-subsequent-turns) |
| `/v1/responses` buffered + streaming | `openai_chat.c agent_execute_messages` | buffered (streaming replays) | S6 (restore-resend / 5xx-disable) |
| `/v1/chat/completions`, `/v1/completions` | `chat_stream_handler`, `completion_stream_handler` | — | **out of scope** (no economizer shadow seam; not a primary-agent reduction path) |

**S7 (OpenAI streaming) — subsumed by S6 (verified repo-wide).** A repo-wide search
confirms `agent_http_post_stream` (true token-by-token upstream streaming) exists **only**
in `anthropic_http.c` — there is no OpenAI-family true upstream streaming anywhere in the
server. The `/v1/responses` streaming handler (`responses_stream_handler`) buffers upstream
via `agent_execute_messages` then replays as SSE, so it inherits S6's full buffered
mutation including the 4xx restore-resend — strictly better than the streaming
disable-only contract — so no separate S7 code was written. A code comment in
`responses_stream_handler` records this as a regression guard.

**Acceptance:**
- **#1–#6 (mechanical + integration): met.** Unit tests: `test_config_economizer`
  (flags, mutate-auto-enable-not-persisted, ttl≤0 rejected), `test_msg_session_disable`
  (LRU/TTL/sweep/eviction + key resolve + 4-case cross-tenant matrix),
  `test_gateway_mutate` (should_apply every bypass class, snapshot OOM-safety, replace
  ownership, provenance), `test_gateway_mutate_wire` (4xx-restore-resend / 5xx-disable /
  streaming disable / error-frame + status classification / token-delta sampling /
  no-behavior-change byte-identical). Full `unit-tests` + CI `build`/`build-integrity`
  green. Provenance §2.6 matrix covered across `test_gateway_mutate` +
  `test_gateway_mutate_wire` + `test_context_reduce` (gateway→delegate hand-off).
- **Runtime `.253` smoke:** the production `aimee-server` binary was run on the `.253`
  deployment host: `reduce.gateway_mutate=1` + `ttl=0` → **startup-fatal** (exit 1 + the
  config error); `reduce.gateway_mutate=1` + valid ttl → **clean start** (server stayed
  up, the auto-enable-seam WARN fired, no fatal). Validates the config/startup wiring on
  the real binary.
- **#7 (`.254` ≥7-day ≥10k/provider/mode live validation) — carried.** Deployment tier;
  gates the separate **default-ON** decision (R6, out of scope), and needs a priced
  provider + real workload for the net-token and 4xx-parity gates.
- **#8 (snapshot-cost benchmark) — carried.** Hardware tier (O2, perf-owned).
- **O1 global flood-disable — deferred by design** (§9).

Everything is **default-OFF** with a proven zero-behavior-change gate, so this ships the
mechanism; the default-ON flip is a separate, data-gated decision.
