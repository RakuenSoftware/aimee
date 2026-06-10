# Proposal: finish the first-class `/v1` migration residual cleanup

- **State:** done
- **Status refreshed:** 2026-06-09
- **Split:** functional `/v1` dispatch migration, `cli_v1_dispatch_local`, retired `/v1/rpc`, and kb-intelligence ownership gate moved to `docs/proposals/done/v1-dispatch-migration.md`.

## Shipped

- Stale comments and strings that mentioned `POST /v1/rpc` as an active bridge
  were removed or converted to explicit historical notes.
- The current contract is explicit: dispatch uses first-class `/v1` routes, with
  generated client route maps and route coverage gates keeping the map in sync.
- Trust-model wording around `CAPS_ALL` now describes per-method `/v1`
  capability checks and bearer scope:
  - UDS is same-user trusted and gets `CAPS_ALL`.
  - Unscoped TCP bearers get `CAPS_AUTHENTICATED`.
  - Scoped TCP bearers get query-only read caps.
  - `aimee.api.remote_writes=data` opens only data-plane writes under caps.
  - `aimee.api.remote_writes=full` grants the fully trusted delegate/tool/control
    tier.
- Updated audit candidates: `scripts/check-v1-method-coverage.py`,
  `scripts/gen-cli-v1-routes.py`, `scripts/check-cli-v1-routes.py`,
  `src/server/server_http.c`, `src/server/server_main.c`,
  `src/server/server_http_routes.inc`, generated client route comments, docs, and
  older test comments.
