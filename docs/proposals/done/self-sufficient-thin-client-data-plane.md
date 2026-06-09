# Proposal: self-sufficient thin client data plane

- **State:** done
- **Status refreshed:** 2026-06-09
- **Split from:** `docs/proposals/pending/self-sufficient-thin-client.md`

## Shipped

The thin-client invariant and data-plane phases are implemented.

- The thin client is DB-free and speaks first-class `/v1` routes to `aimee-server`.
- Remote endpoint discovery is available through the client transport and remote configuration path.
- Data/read commands are routed through generated method-to-`/v1` maps.
- `/v1/workspaces`, `/v1/runner/poll`, and `/v1/runner/respond` exist as the detached-workspace reverse-channel foundation.
- `aimee workspace serve <id>` can poll for workspace operations and post results back to the server.

## Verification Notes

Verified in-tree evidence: `src/Makefile`, `src/cli_v1_routes_gen.inc`, `src/posix/cli_client.c`, `src/cli_workspace_serve.c`, and `src/server/openapi_server_data.h`.
