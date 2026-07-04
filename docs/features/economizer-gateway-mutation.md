# Economizer gateway mutation (primary-agent context reduction)

The context economizer reduces the **delegate** (sub-agent) turn loop by default. The
inbound `/v1` **gateway** — the path that serves the **primary** agent, including
Claude-Code-through-aimee and Codex-through-aimee — historically ran the economizer in
**shadow** (`measure_only`): it measured the reduction opportunity but never applied it.

**Gateway mutation** makes the gateway *apply* the reduced messages to the live request,
so the primary agent's tokens are reduced too. It is **compress-only**, **default-OFF**,
and guarded by a per-session circuit breaker so a bad reduction cannot *persistently*
break live client traffic.

## Configuration

```yaml
reduce:
  gateway_seam: true                       # existing: shadow / measure on /v1 (baseline)
  gateway_mutate: false                    # NEW: apply the reduction. DEFAULT OFF.
  gateway_session_disable_ttl_ms: 3600000  # NEW: circuit-breaker window (1h). MUST be > 0.
```

- `gateway_mutate: true` **implies** `gateway_seam` — config load auto-enables the shadow
  seam **in memory** (never rewriting your file) and logs one WARN, so the shadow baseline
  the validation gates compare against always exists.
- `gateway_session_disable_ttl_ms` **must be > 0**. `0`/negative is a **startup-fatal**
  config error (the server refuses to start): a permanent-pin / disabled breaker on the
  live path is disproportionate runtime risk.

## What it does, per path

- **Buffered** (`/v1/messages` stream:false, `/v1/responses`): snapshot the pristine
  messages → reduce → apply. On **any upstream 4xx** the gateway restores the pristine
  original, repairs it, **resends once**, and disables the session for subsequent turns.
  On **5xx** it disables the session **without** resending and forwards the 5xx as-is.
- **Streaming** (`/v1/messages` stream:true): the HTTP 200 is committed to the client on
  the first byte, so there is **no mid-stream retry**. The reduced stream is sent; if an
  **invalid-request-class** SSE error frame (`invalid_request_error` / `request_too_large`)
  is seen — inspected at the SSE decoder layer and forwarded unchanged — the session is
  disabled for **subsequent** turns. Rate-limit / overloaded / auth frames are forwarded
  **without** disabling. The current turn always completes as the upstream delivered it.
- **OpenAI `/v1/responses` streaming** buffers upstream (via the same buffered path) then
  replays as SSE, so it inherits the **buffered** treatment — including the 4xx
  restore-resend. aimee has no true token-by-token upstream streaming for the OpenAI
  primary path, so there is no OpenAI-specific streaming disable-only code.

## The per-session circuit breaker

Mutation is attempted only for a **resolvable per-identity session key**, derived (in
order) from a validated `aimee-session-id` header (`== SHA-256(auth_identity)[0..16)`),
else `SHA-256(bearer)[0..16)`. A request with **neither** is a pristine passthrough that
writes **no** disable state — so one caller's failure can never disable reduction for an
unrelated caller. Disable state is a bounded (10k), TTL'd, process-local set.

## Safety contract

- `reduce_gateway_mutate: false` ⇒ the inbound request is **byte-identical** to today.
- The gateway **never dispatches a reduced payload it cannot restore** to the pristine
  original (snapshot-first; OOM ⇒ hard-bypass).
- A reduction that does not net-shrink, would split a tool_use/tool_result pair, or the
  reducer errored on ⇒ **hard-bypass** (forward pristine), recorded in
  `gateway_hard_bypass{reason}`.
- Provenance (`reduce_state.reduced`) is set only after the reduced payload is installed
  and cleared on every hard-bypass / restore / OOM path.

## Telemetry (validation input)

`gw_stat_dump` emits (Prometheus-ish): `gateway_mutate_attempted`, `…_applied`,
`gateway_hard_bypass{reason}`, `gateway_4xx_restore_resend`, `gateway_5xx_disable`,
`gateway_stream_error_disable`, `gateway_session_disabled_set{reason}`,
`gateway_session_disabled_blocks`, and the sampled `gateway_token_delta_*` sums.

The token-delta is sampled **deterministically at 1-in-100** applied mutations
(`gateway_token_delta_sample_count`, `…_baseline_sum`, `…_reduced_sum`). The `.254`
net-shrink gate checks that the sampled `reduced_sum < baseline_sum` (sampled mean
reduced below mean baseline); the sums are published before the count so a scrape never
sees a count ahead of its sums.

## Rollout & rollback

Enable `reduce.gateway_mutate` on a validation host, watch the counters, and roll back by
flipping it to `false` (the circuit breaker also self-limits per session). The
default-**ON** decision, the ≥7-day `.254` validation campaign, and the snapshot-cost
benchmark are **out of scope for this feature** and gate that separate decision.
