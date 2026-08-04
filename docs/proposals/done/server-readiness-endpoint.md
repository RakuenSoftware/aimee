# Proposal: a real readiness endpoint for aimee-server

- **State:** DONE — archived after the readiness endpoint shipped.
- **Historical state:** PENDING — design only, no code in the proposal PR.
- **Author:** JBailes
- **Date:** 2026-07-22
- **Charter roles:** Constrain-Verify (report measured dependency state instead
  of an unconditional constant), Enforce (fail closed: unknown state is
  not-ready, never ready).
- **Review:** adversarially reviewed 2026-07-22 (codex + MiniMax-M3, 2 seats, no
  degradation). The review dropped two of the three slices in the original
  draft; see [§7 Rejected](#7-rejected-with-reasons). This document is the
  surviving slice, redesigned against the review's blocking findings.

## Thesis

`route_health()` (`src/server/server_http.c:657-661`) is:

```c
snprintf(resp, (size_t)cap, "{\"status\":\"ok\",\"service\":\"aimee-server\"}");
return 200;
```

It is a string literal. It returns `"ok"` when DB1 is unwritable, when aimee-kb
is unreachable, and when aimee-llm is down. Anything polling it — a container
restart policy, an uptime check, an operator triaging an incident — reads a value
that carries no information about whether the server can actually serve.

A health endpoint that cannot report unhealth is not a missing feature. It is a
false signal, and it is worse than having no endpoint, because it is trusted.

## §0 What already exists

Every claim below was verified by direct read at the cited location.

- **`/v1/health` is the only such route.** `src/server/server_http_routes.c:1505`
  registers `GET /v1/health -> rh_health`. There is no `/v1/ready`, no
  `/v1/live`. The only other match for "readiness" anywhere on the server route
  table is `/v1/calibration/readiness` (`:1732`), which is unrelated — it
  concerns model calibration, not service health.
- **aimee-kb has the same shape.** `src/kb/http/kb_http.c:3` documents it serving
  `/v1/health`, `/v1/version`, `/v1/capabilities`, handled at `:160` and `:988`.
  The `route_health` comment in `server_http.c:651-655` states the server surface
  deliberately mirrors kb "so a client can probe either service the same way."
  Whatever this proposal does, it must not silently break that symmetry.
- **Nothing on the server reports dependency state.** A search across
  `src/server/` for dependency/degraded health reporting found only unrelated
  uses: `server_pipeline.c:75,80` (per-response degradation flags),
  `token_tracker*.c` (link-dependency comments), `server_state.c:397` and
  `server_mcp_call_table.c:826` (unrelated `dependency_count` loops).
- **A check framework already exists, in the CLI.** `src/cmd_doctor.c` has a
  `CHECK_OK`/`CHECK_WARN` status enum (`:26-27`), many per-check evaluations, and
  JSON emission through `status_label()` (`:974`, `:1021`, `:1108`).
- **The constant is deliberate, and this is the key constraint.**
  `src/server/server_http.c:685` keeps a "dependency out of this unit and its
  test," and `src/server/server_api.c:3-6` states the listener "is kept
  dependency-light: native resources backed ... keeping those dependency
  closures out of `server_http.c` and its unit test." `route_health` is not a
  constant by oversight; it is a constant because `server_http.c` is deliberately
  isolated from dependency closures. **A fix that calls dependency checks
  directly from `server_http.c` would regress that design.**

## §1 Design

### §1.1 Liveness and readiness are separate endpoints

The review's first blocking finding was that the original draft left the
liveness-versus-readiness contract undecided, which determines whether an
orchestrator restarts a healthy process during a transient dependency outage.
Deciding it:

- **`GET /v1/health` stays exactly as it is.** Unconditional `200`, constant
  body. It means "this process is alive and serving HTTP." That is a truthful
  liveness signal, it preserves the documented kb symmetry, and it breaks no
  existing consumer. A restart policy pointed at it keeps working and keeps
  being correct: a live-but-not-ready process should *not* be restarted.
- **`GET /v1/ready` is new.** It means "this process can serve work right now,"
  and reports a per-dependency breakdown with an overall roll-up.

This split is the whole answer to the draft's unresolved "200 vs 503" question.
`/v1/health` never returns 503. `/v1/ready` returns `200` when ready and `503`
when not, which is what orchestrators and load balancers expect from a readiness
probe. No existing caller changes behavior, because no existing caller polls a
route that does not yet exist.

### §1.2 Execution model: cached snapshot, refreshed off the request path

The review's second blocking finding was that "bounded by timeout" is not the
same as "non-blocking" — a synchronous check still blocks until its timeout, and
reusing a status enum provides no execution model.

`/v1/ready` never performs I/O on the request path. It serves a cached snapshot:

- A background refresher samples each dependency on a fixed interval and
  publishes an immutable snapshot (per-dependency status, last-sample time,
  last error).
- The handler reads the snapshot and returns it with a `sampled_at` timestamp and
  an `age_seconds` field, so a consumer can tell a fresh "ready" from a stale one.
- **Startup:** before the first sample completes, state is `unknown` and the
  endpoint reports **not-ready / 503**. Fail closed — an unsampled dependency is
  never reported ready.
- **Staleness:** if a snapshot is older than a configured staleness bound, the
  affected dependencies degrade to `unknown` and the roll-up is not-ready. A
  wedged refresher must not look healthy forever.
- **Timeout:** a dependency probe that exceeds its per-probe timeout is recorded
  as `fail` with reason `timeout`, not as ok and not as unknown.
- **Concurrency:** snapshot publication is atomic; concurrent requests either see
  the previous snapshot or the next one, never a partially written one.

### §1.3 The seam

To respect the `server_http.c` isolation constraint (§0), dependency checks are
**not** called from `server_http.c`. The readiness provider is registered through
the same style of seam `server_api.c` already uses to keep dependency closures
out of the listener and its unit test: `server_http.c` holds a provider pointer
and calls it; the implementation lives outside the unit. With no provider
registered, `/v1/ready` reports `unknown`/503 — which keeps `server_http.c`'s
existing unit test dependency-free.

### §1.4 Dependency set

Initial set, each independently reported: **DB1**, **aimee-kb**, and
**aimee-llm** *when configured* (an unconfigured optional dependency is reported
`not_configured` and does not contribute to the roll-up). The roll-up is ready
only if every contributing dependency is `ok`.

## §2 Acceptance criteria

1. `GET /v1/ready` exists and returns a per-dependency breakdown (DB1, aimee-kb,
   aimee-llm-when-configured) plus an overall roll-up, with `sampled_at` and
   `age_seconds`.
2. `GET /v1/health` is byte-identical to its current response and still returns
   an unconditional 200. A test asserts this explicitly, so the liveness contract
   and the kb symmetry cannot regress.
3. **Red before green.** A test harness starts the server with an injected
   failing dependency and asserts `/v1/ready` reports non-ok and status 503.
   The same harness, run against a build where the readiness provider is
   replaced by one that unconditionally reports ok, must **fail** — proving the
   test detects blindness rather than merely observing a hardcoded string. (The
   original draft asked the test to fail against the stock endpoint; that was
   unachievable, since the stock build has no `/v1/ready` to point it at. This
   is the corrected formulation.)
4. Before the first sample completes, `/v1/ready` reports `unknown` and 503.
   A test asserts this startup behavior.
5. A snapshot older than the staleness bound degrades to `unknown`/503. A test
   asserts a wedged refresher does not report ready indefinitely.
6. A dependency probe exceeding its timeout is reported `fail`/`timeout`, and a
   test asserts the handler's own latency stays bounded and independent of that
   probe's duration — i.e. the request path did no I/O.
7. `server_http.c`'s existing unit test gains no new dependency linkage. With no
   provider registered, `/v1/ready` reports `unknown`/503.
8. The route, its schema, and the 200-vs-503 boundary are added to the OpenAPI
   surface, and generated docs regenerate cleanly.

## §3 Non-goals

- **Not** changing `/v1/health`. Its constant response is the correct liveness
  answer and is deliberately preserved.
- **Not** changing aimee-kb's health surface. Adding a matching kb `/v1/ready`
  is a reasonable follow-up but is not in scope, and the kb `/v1/health`
  symmetry is preserved either way.
- **Not** introducing a second check abstraction. Where `cmd_doctor.c`'s checks
  fit the dependency probes, reuse them; where they do not (doctor is a CLI
  one-shot, this is a long-lived sampler), do not contort either side to share
  code that does not want to be shared.
- **Not** adding metrics, alerting, or a dashboard surface for readiness.
- **Not** touching untrusted-content marking or model capability resolution —
  see §7.

## §4 Risks

- **Sampling load.** A background refresher adds periodic traffic to DB1, kb, and
  llm. The interval must be configurable and default conservatively; a readiness
  probe that measurably loads its dependencies is its own outage cause.
- **Over-reporting not-ready.** Fail-closed semantics mean a flapping optional
  dependency can park the server as not-ready. This is why an unconfigured
  dependency reports `not_configured` and is excluded from the roll-up, and it is
  the main thing to watch in the first deployment.

## §5 What would falsify this proposal

If an operator's actual triage path for "is the server working" is
`aimee doctor`, and no orchestrator or automated consumer will ever poll
`/v1/ready`, then this is machinery for a consumer that does not exist and the
correct fix is documentation pointing at `aimee doctor`. This proposal assumes
the containerized deployments in `compose.server*.yaml` want a readiness probe;
that assumption is stated here rather than buried, and should be confirmed
before implementation.

## §6 Validation status

No code in this PR. Every file:line citation was read directly. The design
answers the four blocking findings raised in review (liveness/readiness
contract, status-code matrix, execution model, and the unachievable red-before-
green criterion). Nothing here is validated by execution yet; §2 is the gate.

## §7a Known follow-ups from implementation review

Adversarial review of the implementation (2026-07-22) raised three points that
were accepted as valid but deliberately not fixed in the first change. They are
recorded here so they are not rediscovered as new findings. Note the review ran
**degraded** — one of two seats failed — so these are single-reviewer findings
that were verified against the source rather than accepted on the panel's word.

1. **Sampler lifecycle.** The sampler is a detached thread with an infinite
   loop and no stop/join protocol, and `fork()` behavior is undefined (a child
   inherits `g_ready_thread_started` and possibly a locked mutex without
   inheriting the thread). This matches the existing precedent in
   `src/server/agent_logging.c:121-129`, so it is not a new deviation — but if
   that pattern is ever given a real lifecycle, readiness should move with it.
2. **Is aimee-kb genuinely required?** The roll-up reports not-ready when kb is
   down, which drains the instance even if chat and session paths remain
   serviceable. The proposal committed to "ready only if every dependency is
   ok" and review accepted it, so the implementation follows it — but this is
   the §4 over-reporting risk in concrete form. Resolving it properly needs an
   inventory of which workloads actually require kb, which is a deployment
   question, not a code one. Watch it in first deployment.
3. **Information exposure.** `/v1/ready` is public and unauthenticated (like
   `/v1/health`) but returns named dependencies and outage timing, which is more
   operational detail than the generic liveness probe gives away. Options are a
   minimal public roll-up with details behind authentication, or an explicit
   decision that this is acceptable on the deployment's trust boundary. Not
   resolved here.

A fourth review point — that tests assert on substrings rather than parsing the
JSON — is acknowledged: assertions could pass on a malformed body. The
status/body consistency check added in review closes the case that mattered
(a 200 disagreeing with its own body), but full semantic assertions remain
worth doing.

## §7 Rejected, with reasons

The original draft bundled two further slices. Both were dropped in adversarial
review, and both kills were accepted. Recorded here so the reasoning is not
relitigated:

- **Mark `web_search` results as untrusted.** `web_search_format_results()`
  (`src/server/web_search.c:429-447`) emits results with no untrusted marker,
  while its sibling `web_read` marks every span (`src/posix/web_read.c:377`,
  `:559`, `:615`). **Dropped** because the draft admitted it had not traced the
  downstream path from tool return to prompt assembly, nor the memory-recall
  (`src/cmd_hooks.c`, `src/db2/memory_query.c`) or repo-file-content ingress
  paths. Without that trace the premise — that nothing marks the content
  downstream — is unestablished, and the proposed cross-tool invariant test had
  no machine-readable notion of "returns external content" to test against. To
  revive it: trace every external-content ingress path, reproduce an unmarked
  payload in the stock system, and decide whether external-content provenance
  becomes explicit tool metadata.
- **Probe self-hosted endpoints for model capabilities.** Self-hosted models fall
  through `model_capability_get()` (`src/model_registry.c:868-901`) to
  name-prefix guessing via `g_ctx_windows[]` (`:149`, `:207-211`). **Dropped**
  because the highest-precedence operator override
  (`models_dev_override_lookup`) already solves this by hand, and the draft named
  no OpenAI-compatible response field that reliably supplies a real context
  window — so the probe's central claim was unsupported, while adding a network
  failure surface to every capability resolution. To revive it: name the endpoint
  APIs and schemas, prove they expose context size for the target servers, and
  specify timeout, cache TTL, auth failure, and 4xx-vs-5xx behavior.
- **Deleting the dead `models_dev_capability_get()` stub**
  (`src/models_dev.c:116-123`, unreferenced — the resolution chain uses
  `models_dev_cache_lookup()` instead) is a real but unrelated cleanup. It was
  correctly called out as padding and belongs in its own change with its own
  reference and build verification.
- **Remote MCP configuration and OAuth.** Outbound MCP supports only a static
  bearer (`src/modules/protocols/mcp/mcp_client.c:666-682`). Deferred, and the
  draft's claim that the product gap is *established* was overstated: a search of
  `src/modules/config/` alone does not prove the transport is reachable in a
  packaged runtime. Any future proposal starts by tracing references to
  `mcp_transport_sse_open`, its build-target inclusion, and all configuration
  entry points.
