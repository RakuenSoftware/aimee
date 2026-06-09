# Proposal: first-class `/v1` dispatch migration shipped work

- **State:** done
- **Status refreshed:** 2026-06-09
- **Split from:** `docs/proposals/done/v1-dispatch-migration-finish.md`

## Shipped

The functional migration from the old generic `POST /v1/rpc` bridge to typed first-class `/v1` routes is complete.

- `cli_v1_rpc_local` was renamed to `cli_v1_dispatch_local`.
- The standalone `POST /v1/rpc` endpoint is retired.
- Dispatch methods resolve through generated first-class route maps.
- The kb-intelligence ownership gate exists and is wired into lint.
- `/v1/intelligence/*` routes are either surfaced through the server/client path or explicitly marked `kb-direct`.

## Verification Notes

Verified in-tree evidence: `src/cli_rpc_routes.inc`, `src/server/server_http_routes.inc`, `src/tests/test_server_http.c`, `scripts/check-kb-intelligence-surfaced.py`, and `src/Makefile`.
