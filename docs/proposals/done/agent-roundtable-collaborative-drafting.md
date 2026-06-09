# Proposal: Agent roundtable shipped work

- **State:** done
- **Status refreshed:** 2026-06-09
- **Split from:** `docs/proposals/pending/agent-roundtable-collaborative-drafting.md`

## Shipped

The engine and first-class entry points described by the original proposal have landed.

- The ensemble aggregate and roundtable routes are first-class `/v1` routes: `POST /v1/delegate/aggregate` and `POST /v1/delegate/roundtable`.
- Thin-client dispatch maps `aimee delegate aggregate` and `aimee delegate roundtable` to those typed routes and polls `/v1/runs/{id}`.
- `delegate_roundtable_run` and roundtable result types exist in `delegate_ensemble`.
- `agent_task_t` can select a named participant, and the runtime has explicit per-task temperature support.
- Named participant execution clones the selected agent before mutation, avoiding the old shared-agent parallel mutation path.
- Roundtable config keys are represented in `config_t` and parsed/saved through the config section plumbing.

## Verification Notes

Verified in-tree evidence: `src/headers/delegate_ensemble.h`, `src/cli_rpc_routes.inc`, `src/server/server_http_routes.inc`, `src/tests/test_server_http.c`, `src/headers/agent_tasks.h`, `src/server/agent_runtime.c`, `src/headers/config.h`, and `src/headers/config_sections.h`.
