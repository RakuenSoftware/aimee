# Aimee IR as the sole path — finish the response half, retire raw-passthrough, and make modules pluggable

- **State:** PENDING (proposed; reviewed at roundtable 2026-07-17) — sequel to the prior
  (completed) canonical-IR proposal, which made the IR the default request-build path. This
  proposal makes the IR the sole core codepath in both directions, retires the legacy translators
  and the raw-passthrough bypasses, and adds a pluggable stage registry so modules (memory,
  delegates, workflows) can be added or removed from the path at will. The roundtable ratifies the
  raw-fast-path tradeoff, the cross-protocol precision caveat, and the stage-registry design points.
  Each slice is independently gated and filed per the repo's proposal lifecycle on completion.
- **Author:** JBailes
- **Date:** 2026-07-17

## Problem

Two codepaths still run in shadow. The canonical IR is the default REQUEST-build path (legacy
demoted to `if (!prov_body) build_provider_body(...)` fallback), but the live system also still
runs the legacy translators AND keeps a raw-passthrough bypass that skips the core. Two paths in
shadow is the most expensive steady state — double maintenance and a whole class of divergence
bugs that only appear on one path.

A live, real streaming bug sits at the un-migrated REQUEST-build seam today:

- Buffered-replay sends the provider body with `stream:true`. Both legacy
  `build_provider_body(...,1)` and the IR build via `openai_backend_build` faithfully carry
  `stream:true`.
- The non-responses branch then does `cJSON_Parse(buf_resp)` — the provider was asked to STREAM and
  the reply is parsed as a single JSON object → `primary provider returned an unparseable reply`.
- Not an IR-vs-legacy difference; both paths hit it. Both are wrong at REQUEST-build.
- Correct: `stream = responses_wire ? 1 : (buffered_replay ? 0 : 1)` — responses/codex genuinely
  needs `stream:true` because its replay parses raw SSE; the buffered-replay non-responses wire
  must carry `stream:false`. Both signals are knowable at REQUEST-build time:
  `responses_wire` from the URL, and `buffered_replay = gateway_prevent_subagents_enabled() ||
  responses_wire`.
- The seam being hand-rolled per driver is why this class exists — fold the flag into the IR
  build and the seam stops being bespoke. This is a live bug and ships on its own before the
  larger response-parse work.

## Current state — the trace (all file:line verified on branch `claude/forge-default-on`)

REQUEST build (client -> provider): DONE, default-ON, legacy demoted to fallback-only.
- `aimee_ir_path_enabled()` returns 1 by default (`src/server/aimee_ir_serve.c:22`; `AIMEE_IR_PATH=0` forces legacy).
- Four request-build seams route through the IR with `if (!prov_body) build_provider_body(...)` fallback:
  - `src/server/anthropic_http.c:384` — buffered `/v1/messages`
  - `src/server/anthropic_http.c:888` — streaming `/v1/messages`
  - `src/server/openai_chat.c:900` — `aimee_ir_build_from_chat` (agent chat path)
  - `src/server/openai_chat.c:1106` — `aimee_ir_responses_to_chat` (`/v1/responses` client ingress)

RESPONSE parse (provider -> client): NOT on the IR at all. This is the real gap.
- `openai_backend_parse` / `anthropic_backend_parse` / `responses_backend_parse` are defined and
  unit-tested but have ZERO live callers (only their own definitions in
  `src/server/aimee_backend_*.c`).
- The live return trip still runs `driver->parse_response -> parsed_response_t ->
  emit_message_as_sse` (the buffered-replay block at `src/server/anthropic_http.c:907+`).

Incremental streaming relay (provider SSE -> client SSE): built but DARK.
- `aimee_ir_stream_relay_enabled()` is DEFAULT-OFF (`AIMEE_IR_STREAM_RELAY`), wired at
  `src/server/anthropic_http.c:1063`. Legacy `anthropic_stream_feed_openai` still drives the
  incremental non-codex OpenAI-chat relay. (Codex/delegate boxes take buffered-replay, which
  bypasses this relay.)

Raw-passthrough bypasses (the thing to eliminate — user directive: eliminate raw-passthrough):
1. Anthropic->Anthropic parity build: both build sites branch
   `if (driver_is_anthropic(driver)) build_anthropic_provider_body(req, ...)` which duplicates
   `req` directly and NEVER enters the IR. This is the same-protocol raw fast-path the
   `src/headers/aimee_ir.h` header defends with the `raw` sidecar.
2. Codex raw-SSE replay: the `raw_responses` branch parses raw SSE TEXT via
   `driver->parse_response(NULL, buf_resp, ...)`; the response is never structured into the IR.
   Load-bearing for codex today; removed in Slice 1 (see below).

Coupling to cut:
- `translate_request(req, driver, ag, &messages, &tools, &system_text)` runs UNCONDITIONALLY at
  `src/server/anthropic_http.c:891` even when the IR build succeeds; its output only feeds the
  legacy-fallback args. Dead work per IR request, and a dependency that blocks deleting
  `build_provider_body`.

Observability — current state (mixed; see Slices 1 + 2 for the gap closure):
- REQUEST-side parity counters ARE observable in the field. `src/server/server_state.c:1454-1464`
  dumps every metric via `aimee_ir_metric_total(...)` under an `ir` object on
  `GET /v1/dashboard/metrics` (`src/server/server_http_routes.c:1576`). The request-side deletion
  gate — `AIMEE_IR_M_BODY_MISMATCH == 0` and `AIMEE_IR_M_LEGACY_FALLBACK == 0` over a stated
  window — is measurable today.
- RESPONSE-side shadow is NOT wired live. `AIMEE_IR_M_RESP_MATCH` / `AIMEE_IR_M_RESP_MISMATCH`
  are only incremented inside `src/server/aimee_ir_shadow.c:155-190`; there is NO live caller
  of a response-shadow observe. Only `aimee_ir_shadow_observe_request` is wired
  (`src/server/anthropic_http.c:424` and `:801`). So response-parse parity is currently
  UNMEASURABLE on live traffic — and that is the metric that gates Slice 1 (response-parse) and
  the response-translator deletion.
- `aimee_ir_metric_get` (per-WIRE granularity, `src/headers/aimee_ir_metrics.h:29`) still has no
  non-test caller; the dashboard uses `aimee_ir_metric_total` (summed across wires). Per-wire
  breakdown is a minor nice-to-have, not a blocker.

## Target architecture

The IR (`aimee_request_t` / `aimee_response_t`, `src/headers/aimee_ir.h`) becomes the SOLE core
codepath in BOTH directions. No client-shape->provider-shape translation, and no same-protocol
raw-passthrough that skips the core. Same-protocol correctness is enforced by committing to IR
byte-fidelity — which shadow mode already measures via `ir_rebuild_mismatch` — rather than by
keeping a bypass.

**Roundtable-decision framing.** Dropping the raw fast-path trades a small same-protocol
re-serialize cost + the cJSON-double precision caveat for a single codepath. State the tradeoff;
let the table rule. Until the table ratifies, the raw fast-path remains in tree as fallback
behind `AIMEE_IR_PATH=0` (mirrors the parent's rollout discipline).

## Pluggable module stages (NEW — first-class requirement)

Cross-cutting features become STAGES over the IR that can be added or removed from the path at
will: memory injection, delegates, workflows, guardrails. A stage is a pure
`aimee_request_t -> aimee_request_t` (request stages) or `aimee_response_t -> aimee_response_t`
(response stages) transform, wire-agnostic, operating on typed blocks (`aimee_message_t` /
`aimee_block_t`) regardless of the client or provider protocol.

- A registry orders stages and lets them be toggled by config (and ideally at runtime) without
  touching translation code. Adding a module = registering a stage; removing = unregistering.
- Contrast with today: a feature that must understand provider JSON is implemented once per wire
  format. As IR stages it is implemented once, period.

### Design decisions (roundtable-ratified)

- **Trust boundary at backend-build, not at stage chain.** The opaque tool id / tool name rule
  (per `aimee_ir.h`) is enforced at the BACKEND-BUILD boundary — the last step before the IR
  leaves the proxy. All stages see and may stage opaque ids/names freely; a misordered stage
  cannot leak raw client tool ids into provider JSON because the backend adapter re-validates /
  re-emits opaque at the seam. This is not negotiable per stage.
- **Ownership: stages take BORROWED const IR and RETURN an owned IR.** A request stage
  `f(const aimee_request_t *in, aimee_request_t *out)` borrows its input and writes a fresh
  owned output (or returns the input by pointer when no-op). Response stages mirror this.
  The registry owns the chain lifetime: it frees the prior IR before invoking the next stage
  when the stage returns a new owned allocation. No implicit sharing, no implicit free by the
  stage.
- **Streaming interaction: do NOT re-allocate the full IR per stage per SSE chunk.** Stages
  registered for the incremental-streaming path operate on the IR-DELTA surface (turn_start /
  block_start / block_delta / block_stop / turn_stop, per the parent's Q3 ruling), not on the
  buffered `aimee_request_t` / `aimee_response_t`. Buffered stages run once before the relay;
  delta-aware stages run per chunk. The registry distinguishes `STAGE_BUFFERED` from
  `STAGE_DELTA` at registration time; mixing them is a registration error caught at startup.

Open design points for the table:
- Stage ordering semantics and guarantees (strict ordering, allow-list per stage).
- Whether third-party/plugin stages are in scope or core-only for v1.
- Failure semantics: a stage that errors — fail-closed vs skip.
- How stage toggles interact with the shadow/parity gate (a toggled-off stage must not be
  observable in parity metrics; otherwise its absence is masked).

## Slices (each: pure cores + tests first, then wire, roundtable before PR — mirror the parent's discipline)

1. **Fix the buffered-replay stream flag at request-build.** Compute
   `stream = responses_wire ? 1 : (buffered_replay ? 0 : 1)` inside the IR build path
   (`openai_backend_build`, reached by the already-IR-routed request seams at
   `src/server/openai_chat.c:900`, `:1106`, and `src/server/anthropic_http.c:384`, `:888`),
   where `buffered_replay = gateway_prevent_subagents_enabled() || responses_wire`. The
   Anthropic-parity build is NOT touched here — it does not route through the IR until Slice 5,
   which carries this same stream-flag rule to that path when it lands.
   Standalone slice — this is a live bug, independently shippable, and must not wait on the
   larger response-parse migration. **Slice 1 owns the stream-flag deterministic tests for
   BOTH wires:** (a) non-responses wire + `gateway_prevent_subagents_enabled() == 1` carries
   `stream:false`; (b) responses / codex wire carries `stream:true`. Slice 3 only asserts
   these still pass. Confirms the bug class won't reappear when other drivers route through
   the IR build.

2. **Wire the response-side shadow into the live response path.** Add a live caller of
   `aimee_ir_shadow_observe_response(...)` next to the existing
   `aimee_ir_shadow_observe_request` sites so `AIMEE_IR_M_RESP_MATCH` /
   `AIMEE_IR_M_RESP_MISMATCH` are incremented on real traffic. Surface already handled by the
   existing `ir` dashboard block (`src/server/server_state.c:1454-1464`) — do NOT reimplement
   request-side observability, which is already live. Unblocks evidence-gating for Slice 1.

3. **Response-parse through the IR.** Wire `*_backend_parse` into the live response path,
   replacing `driver->parse_response -> parsed_response_t -> emit_message_as_sse` with
   `provider-JSON -> IR -> emit`. PHYSICAL DELETION of the codex `raw_responses` branch happens
   here and ONLY here — this slice owns the response-parse seam where codex SSE becomes IR.
   **Done-gate for the codex bypass removal:** Slice 1's codex-replay deterministic test
   passes (stream flag set correctly on the responses wire), `AIMEE_IR_M_RESP_MISMATCH == 0`
   over a stated window (e.g. 24h / N requests) during the shadow run, and a deterministic
   codex-replay test passes end-to-end.
   **Abort / rollback clause:** if `AIMEE_IR_M_RESP_MISMATCH` is nonzero during the shadow
   run, revert the response-parse default-on without touching later slices. Largest slice.

4. **Flip `AIMEE_IR_STREAM_RELAY` on** once shadow divergence stays clean on live non-codex
   streaming; retire `anthropic_stream_feed_openai`. Gated on
   `AIMEE_IR_M_BODY_MISMATCH == 0` over a stated window on live traffic (Slice 2 already
   covers the response-shadow wire-up for this metric).

5. **Route the Anthropic-parity build through the IR** (remove raw-passthrough bypass #1).
   Applies the Slice 1 stream-flag rule to this path as it lands. Gated on Slice 4's metric.

6. **Delete the legacy fallbacks** (`build_provider_body`, the unconditional
   `translate_request`) once parity metrics are clean on live traffic. Gated on
   `AIMEE_IR_M_BODY_MISMATCH == 0` AND `AIMEE_IR_M_LEGACY_FALLBACK == 0` over the stated
   window. The codex `raw_responses` bypass is already gone (removed in Slice 3); this slice
   does not touch it.

7. **Introduce the pluggable stage registry + first port (memory).** Registry + ordered
   chain + config-driven toggle + borrows-in / owns-out ownership per the design decisions
   above. Port memory injection as the first consumer; prove toggle-off parity with a
   deterministic test.

8. **Port delegates to stages.** Re-implement the delegate-box feature as a registered stage
   (request and response sides). Deterministic test: toggling delegates off produces the
   same IR parity as the pre-slice baseline.

9. **Port workflows to stages.** Same shape as Slice 8, for the workflow feature. Each port
   is its own slice so a regression is local and a partial rollout is shippable.

## Risks / open questions (for the roundtable)

- **Cross-protocol precision.** cJSON numbers are doubles, so cross-protocol re-serialize can lose
  precision on tool-arg integers >2^53 (documented in `aimee_ir.h`). Same-protocol byte-fidelity
  is the mitigation for the parity-build change; cross-protocol precision remains future work.
- **Ordering dependency.** Codex raw-SSE replay (bypass #2) is removed in Slice 3 (response-parse),
  not earlier.
- **Deletion is evidence-gated, not hope-gated.** Request-side deletions gate on
  `AIMEE_IR_M_BODY_MISMATCH == 0` and `AIMEE_IR_M_LEGACY_FALLBACK == 0` over a stated window
  (e.g. 24h / N requests). Response-side deletions gate on `AIMEE_IR_M_RESP_MISMATCH == 0` over
  the same window. No flip without evidence.
- **Stage registry design points** listed above.
- **Enablement/exit criteria** should mirror the parent: parity metrics clean on live traffic
  before each flip/deletion.

## Acceptance

- Every provider response on the live path is parsed `provider-JSON -> IR -> client` (no
  `driver->parse_response -> parsed_response_t` on the default path).
- The codex `raw_responses` branch is physically deleted; codex is served by the IR response
  parse.
- No same-protocol raw-passthrough that skips the IR; `build_provider_body` and the
  unconditional `translate_request` are gone, gated on
  `AIMEE_IR_M_BODY_MISMATCH == 0` AND `AIMEE_IR_M_LEGACY_FALLBACK == 0` over the stated window.
- The response-side shadow counters (`AIMEE_IR_M_RESP_MATCH` / `AIMEE_IR_M_RESP_MISMATCH`)
  appear on `GET /v1/dashboard/metrics` and `AIMEE_IR_M_RESP_MISMATCH == 0` over the stated
  window on the live default path.
- The buffered-replay stream-flag bug is fixed, with a deterministic test for both wires
  (non-responses buffered-replay → `stream:false`; responses / codex → `stream:true`).
- Memory, delegates, and workflows are registered stages that can be added/removed via config,
  each with its own deterministic toggle-off parity test. Trust-boundary rule (opaque tool
  ids/names) is enforced at backend-build, not at stages.

```yaml acceptance
- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_stream_flag_buffered_replay"}
- {id: 2, tier: mechanical, check: "make unit-tests TEST=test_ir_response_parse_live"}
- {id: 3, tier: integration, check: "curl -sk $AIMEE_API/v1/dashboard/metrics | jq -e '.ir.ir_resp_mismatch == 0'"}
- {id: 4, tier: integration, check: "curl -sk $AIMEE_API/v1/dashboard/metrics | jq -e '.ir.ir_body_mismatch == 0 and .ir.ir_legacy_fallback == 0'"}
- {id: 5, tier: mechanical, check: "make unit-tests TEST=test_stage_registry_toggle_parity"}
```
