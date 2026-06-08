# Proposal: finish the first-class /v1 migration (retire the RPC vestiges, close the dispatch gaps)

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter role(s):** none (transport/architecture cleanup — no new store, no new
  DB tier, no intelligence pass).
- **Scope:** `src/cli_rpc_routes.inc` (rename the legacy `cli_v1_rpc_local`
  helper; resolver comments), `src/server/server_http*.c` + `src/headers/*.h`
  (stale `/v1/rpc` doc/comment purge; CAPS trust-model wording), the kb
  intelligence routes (`src/kb/http/kb_http.c` + `src/server` dispatch +
  `cli_rpc_routes.inc`) that are not yet on the dispatch surface, the coverage
  gates (`scripts/check-cli-v1-routes.py`, `check-v1-method-coverage.py`,
  `check-kb-v1-coverage.py`), docs. No new long-lived service.

## Summary

The move from the synchronous `POST /v1/rpc` bridge to first-class /v1 routes is
**functionally complete but not finished**. The CLI forward path is already
strict-/v1 (`cli_rpc_forward` resolves every dispatch method to a generated
route, enforced by the coverage gates; the standalone bridge endpoint is gone).
What remains is **debt and gaps**, not transport work:

- **(A) Vestiges.** The helper that *used* to be the bridge is still named
  `cli_v1_rpc_local` and several comments/headers still say "POST /v1/rpc" even
  though the code now resolves first-class routes. The trust-model wording
  (`CAPS_ALL` "opens the /v1/rpc bridge") describes a thing that no longer
  exists. This misleads readers (it misled a contributor into thinking a new
  command needed "an RPC route").
- **(B) Dispatch gaps.** A whole family of kb capabilities — the
  `/v1/intelligence/*` routes (`calibration/readiness`, `demotion/check`,
  `bandit/export`, `bandit/replay-record`) — exists only as **kb HTTP endpoints
  with dead `cmd_*` handlers**. They were never added to the CLI dispatch
  registry (`cli_rpc_routes.inc`), so they are unreachable from the thin client.
  Their `cmd_kb` handlers (e.g. `kb_cmd_bandit`) are compiled but **not linked
  into the thin client** (which is DB-/`kb_client`-free by design), so
  `aimee kb bandit --export` returns "no typed server RPC route".

Finishing the migration means: delete the vestiges so "strict /v1" is the
*stated* contract, and route the orphaned capabilities so nothing has to reach
for a `cmd_*` handler or a non-/v1 path.

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
  over `/v1/intelligence/bandit/export`) appears to need bespoke plumbing,
  because the intelligence family was never given dispatch routes — when the fix
  is the same `cli_rpc_routes.inc` registry entry every other command uses.

## Current state (verified)

- `cli_rpc_forward` (`cli_rpc_routes.inc:6726`) — strict /v1, no bridge; resolves
  via the generated sync map / async set / `{id}`-path map; gated by
  `check-cli-v1-routes.py` + `check-v1-method-coverage.py`.
- `cli_v1_rpc_local` (`cli_rpc_routes.inc:6688`) — same first-class resolution;
  legacy name only. ~15 live callers (cli_main agent-setup, cli_session_start,
  cli_workspace_serve, cmd_trigger, cli_mcp_serve, gateway_ctx, provider_catalog).
- The `POST /v1/rpc` endpoint is retired (`server_http_routes.inc:262`,
  `server_http.c:895`).
- **Orphaned dispatch gaps:** `kb_http.c:773–806` serves four `/v1/intelligence/*`
  routes with no entry in `cli_rpc_routes.inc` and only dead `cmd_kb` handlers.

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

### Part B — close the dispatch gaps (the intelligence family)

For each orphaned `/v1/intelligence/*` capability, add the standard dispatch
wiring so the thin client reaches it like any other command:

1. A registry row in `cli_rpc_routes.inc` (`{cmd, sub, method, …}`), its client
   marshaller, and the server-side dispatch arm that calls the kb client.
2. Regenerate `cli_v1_routes_gen.inc` via `scripts/gen-cli-v1-routes.py`; clear
   any entry from the coverage gates' EXCLUDED set.
3. Remove the now-redundant dead `cmd_*` handler (or reduce it to the thin
   presenter the dispatch response feeds).

This unblocks the **`aimee optimize`** verb (companion proposal
`optimization-surface.md`) and, with it, `kb calibrate` / `kb demote` over the
thin client — all of which are currently unreachable for the same reason.

### What this deliberately does **not** do

- No change to the /v1 transport, the coverage-gate mechanism, or the async
  run-and-poll model — those are done and correct.
- No reintroduction of a generic dispatch endpoint; every method stays a typed
  route.

## Phasing

- **P1** — Part A rename + comment/doc purge + trust-model wording. Pure cleanup;
  gated by the existing coverage checks (must stay green).
- **P2** — Part B for the bandit surface (`bandit/export`, `bandit/replay-record`)
  — the slice that unblocks `aimee optimize`.
- **P3** — Part B for `calibration/readiness` + `demotion/check`; retire the dead
  `cmd_kb` intelligence handlers.
- **P4** — audit sweep: assert (via a gate) that every kb `/v1` route is either
  in the dispatch registry or explicitly annotated kb-direct, so no future
  capability can be added off-surface.

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
