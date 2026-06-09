# Proposal: finish the first-class /v1 migration (retire the RPC vestiges, keep typed routes)

- **State:** **mostly shipped** — only the Part A.2 comment/doc purge remains;
  see *Status* below.
- **Author:** JBailes
- **Date:** 2026-06-08 (status refreshed after #122/#128-era work)

## Status (verified against `testing` @ #128)

Two of this proposal's three deliverables have since merged; only the cosmetic
comment purge is left.

- **Part A.1 (rename) is done.** `cli_v1_rpc_local` was renamed to
  `cli_v1_dispatch_local` in #122 (the v1-vestige-cleanup that is the parent of
  this work). The "Current state" section below still describes the *old* name as
  pending — that is stale; treat the rename as complete.
- **Part B (kb-direct ownership gate) is done.** `scripts/check-kb-intelligence-surfaced.py`
  now enforces that every `/v1/intelligence/*` route is either surfaced through an
  aimee-server client path or explicitly marked `kb-direct`, wired into `make lint`
  as `kb-intelligence-surfaced-check`. That is the gate Part B asked for.
- **Residual (still open): Part A.2 only** — a handful of stale `POST /v1/rpc`
  comments/strings remain (e.g. `src/headers/cli_client.h`, `src/server/server_http.c`)
  and the `CAPS_ALL` "opens the /v1/rpc bridge" wording. Pure comment/doc cleanup,
  no behaviour change.
- **Charter role(s):** none (transport/architecture cleanup — no new store, no new
  DB tier, no intelligence pass).
- **Scope:** `src/cli_rpc_routes.inc` (rename the legacy `cli_v1_rpc_local`
  helper; resolver comments), `src/server/server_http*.c` + `src/headers/*.h`
  (stale `/v1/rpc` doc/comment purge; CAPS trust-model wording), and the
  coverage gates (`scripts/check-cli-v1-routes.py`,
  `check-v1-method-coverage.py`, `check-kb-v1-coverage.py`) so typed /v1 remains
  the only route contract. No new long-lived service and no new dispatch/RPC
  surface.

## Summary

The move from the synchronous `POST /v1/rpc` bridge to first-class /v1 routes is
**functionally complete but not finished**. The CLI forward path is already
strict-/v1 (`cli_rpc_forward` resolves every dispatch method to a generated
route, enforced by the coverage gates; the standalone bridge endpoint is gone).
What remains is **debt and policy clarity**, not transport work:

- **(A) Vestiges.** The helper that *used* to be the bridge is still named
  `cli_v1_rpc_local` and several comments/headers still say "POST /v1/rpc" even
  though the code now resolves first-class routes. The trust-model wording
  (`CAPS_ALL` "opens the /v1/rpc bridge") describes a thing that no longer
  exists. This misleads readers (it misled a contributor into thinking a new
  command needed "an RPC route").
- **(B) Route ownership drift.** Some kb intelligence capabilities are
  intentionally kb-direct typed endpoints (`/v1/intelligence/*`) rather than
  aimee-server dispatch methods. That split must stay explicit so future work
  does not add `cli_rpc_routes.inc` rows or server dispatch arms merely to make a
  command reachable. Thin-client surfaces should call the appropriate typed /v1
  route directly.

Finishing the migration means: delete the vestiges so "strict /v1" is the
*stated* contract, and document route ownership so nothing reaches for a `cmd_*`
handler, a new dispatch method, or a non-/v1 path.

## Motivation

"Is it not all gone, in preference for the /v1 API?" — almost. The transport is
/v1; the confusion is that the code still *talks* like the bridge exists, and a
few capabilities silently never crossed over. Both are cheap to fix and both
remove real foot-guns:

- A reader can't tell from `cli_v1_rpc_local` / "co-located /v1/rpc bridge"
  comments that there is no bridge. (See `cli_rpc_routes.inc:6681`,
  `provider_catalog.c:342,358`, `server_http.c:315`, `config.h:894`,
  `cli_client.h:41`.)
- A new thin-client command for an existing kb endpoint (e.g. `aimee optimize`
  over `/v1/intelligence/bandit/export`) should use the typed kb V1 route
  directly rather than creating an aimee-server dispatch proxy.

## Current state (verified)

- `cli_rpc_forward` (`cli_rpc_routes.inc:6726`) — strict /v1, no bridge; resolves
  via the generated sync map / async set / `{id}`-path map; gated by
  `check-cli-v1-routes.py` + `check-v1-method-coverage.py`.
- `cli_v1_rpc_local` (`cli_rpc_routes.inc:6688`) — same first-class resolution;
  legacy name only. ~15 live callers (cli_main agent-setup, cli_session_start,
  cli_workspace_serve, cmd_trigger, cli_mcp_serve, gateway_ctx, provider_catalog).
- The `POST /v1/rpc` endpoint is retired (`server_http_routes.inc:262`,
  `server_http.c:895`).
- **Typed kb-direct routes:** `kb_http.c:773–806` serves `/v1/intelligence/*`
  routes directly. These are not dispatch gaps; they are kb-owned V1 endpoints.

## Design

### Part A — retire the vestiges (no behaviour change)

1. **Rename** `cli_v1_rpc_local` → `cli_v1_dispatch_local` (it dispatches a
   pre-marshalled `{method,…}` to its first-class route). Update all callers and
   the test shim.
2. **Purge** stale `/v1/rpc` comments/strings across `server_http*.c`,
   `provider_catalog.c`, `gateway_ctx.c`, `cli_client.h`, `posix/cli_client.c`,
   `windows/cli_client.c`, `cli_mcp_serve.c`. Keep one historical note in the
   route registry header explaining the retirement, delete the rest.
3. **Reconcile the trust model.** `CAPS_ALL` no longer "opens the /v1/rpc
   bridge"; restate it in terms of per-method caps on the /v1 surface
   (`server_http.c:315`, `headers/server.h:89`, `config.h:894`).

### Part B — make kb-direct ownership explicit

For each `/v1/intelligence/*` capability, keep the V1 route typed and explicit:

1. Document whether the endpoint is kb-direct only or mirrored through
   aimee-server for a specific reason.
2. Add a gate that fails if kb-direct endpoints are accidentally surfaced through
   `cli_rpc_routes.inc`, server dispatch arms, or `/v1/rpc` fallback text.
3. Keep thin-client presenters small and route them to the typed V1 endpoint they
   actually need.

This keeps **`aimee optimize`** and `aimee kb ...` surfaces on typed V1 APIs
without adding RPC/dispatch methods.

### What this deliberately does **not** do

- No change to the /v1 transport, the coverage-gate mechanism, or the async
  run-and-poll model — those are done and correct.
- No new dispatch methods for kb intelligence endpoints; every operation stays a
  typed V1 route.

## Phasing

- **P1** — Part A rename + comment/doc purge + trust-model wording. Pure cleanup;
  gated by the existing coverage checks (must stay green).
- **P2** — Part B for the bandit surface (`bandit/export`, `bandit/replay-record`):
  document kb-direct ownership and keep `aimee optimize` on typed V1 calls.
- **P3** — Part B for `calibration/readiness` + `demotion/check`: document
  kb-direct ownership and keep `aimee kb` presenters aligned with those routes.
- **P4** — audit sweep: assert (via a gate) that every kb `/v1` route is either
  intentionally kb-direct or intentionally mirrored, and that neither case adds
  `/v1/rpc` fallback or surprise dispatch surface.

## Risks

- **Coverage-gate churn.** Part B touches the generated route map; regen + the
  three `check-*-v1-*.py` gates must stay green (and the conformance scanner's
  literal-`/v1/` trap avoided — build paths from prefixes, no stray literals).
- **Rename blast radius.** `cli_v1_rpc_local` has ~15 callers + a test shim; a
  mechanical rename, but must land atomically.
- **Hidden kb-direct intent.** Some intelligence routes may be deliberately
  kb-only (operator-on-the-kb-host); P4's audit must allow an explicit
  annotation rather than force everything onto the thin client.

## Relationship to the optimization-surface proposal

`optimization-surface.md` deferred its `aimee optimize points|baseline|replay`
CLI verb precisely because the bandit surface has no dispatch route (Part B
above). P2 of this proposal is that route; landing it lets the optimize verb be a
normal thin-client command instead of a special case.
