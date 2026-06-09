# Proposal: finish the first-class `/v1` migration residual cleanup

- **State:** pending
- **Status refreshed:** 2026-06-09
- **Split:** functional `/v1` dispatch migration, `cli_v1_dispatch_local`, retired `/v1/rpc`, and kb-intelligence ownership gate moved to `docs/proposals/done/v1-dispatch-migration.md`.

## Remaining Work

- Purge stale comments and strings that still mention `POST /v1/rpc` as an active bridge.
- Keep one historical note where useful, but make the current contract explicit: dispatch uses first-class `/v1` routes.
- Re-check trust-model wording around `CAPS_ALL` so it describes per-method `/v1` caps and bearer scope, not a retired bridge.
- Candidate files from the audit: `scripts/check-v1-method-coverage.py`, `scripts/gen-cli-v1-routes.py`, `src/server/server_http.c`, `src/server/server_main.c`, and older test comments.
