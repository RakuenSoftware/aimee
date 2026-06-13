# Proposal: Agent roundtable residual follow-up

- **State:** done
- **Status refreshed:** 2026-06-13
- **Split:** shipped engine, participant-routing, temperature, config, CLI route, and `/v1/delegate/*` work moved to `docs/proposals/done/agent-roundtable-collaborative-drafting.md`.

## Remaining Work (original)

- Add deeper behavioral tests for multi-round convergence, scoring, failure handling, participant compatibility, and sequential turn order.
- Add or tighten MCP exposure if the roundtable should be callable as a first-class MCP tool, not only through the CLI route surface.
- Validate runtime economics for large panels: budget preflight, per-round deadline enforcement, and degraded-result metadata under partial participant failure.
- Confirm user-facing docs/help describe aggregate versus roundtable behavior after the shipped route and engine changes.

## Completion (2026-06-13)

The follow-up is closed out. Audit of the shipped engine found most items already
landed by subsequent work; the one genuine code gap (partial-failure metadata)
was added:

- **Behavioral tests** — the existing `tests/test_delegate_ensemble.c` already
  covers multi-round convergence, review saturation, sequential turn order
  (`test_roundtable_sequential_uses_named_agents`), min-success degradation,
  cost-cap, preflight, keep-best-not-last, deadline, and cancellation. Added
  assertions pinning the new partial-failure metadata on the all-succeed and
  partial-failure cases (both `aggregate` and `roundtable`).
- **MCP exposure** — already first-class: `delegate.roundtable` is dispatched as
  an MCP op (`src/server/server_mcp.c`) alongside the pipeline tools in
  `src/mcp_tools_pipeline.inc`. No change needed.
- **Runtime economics** — budget preflight (`estimated_round_cost` +
  `ensemble_max_cost_usd`), per-round deadline enforcement (`deadline_ms` checked
  each round), and degraded handling were already implemented. **Added** explicit
  partial-failure metadata: `participants_total` / `participants_failed` on both
  `delegate_ensemble_result_t` and `roundtable_result_t`, populated in
  `delegate_ensemble.c` and surfaced in the `/v1/delegate/{aggregate,roundtable}`
  JSON responses (`server_compute.c`).
- **Docs** — `docs/DELEGATES.md` now documents aggregate vs roundtable behavior
  and a partial-failure/degradation metadata table.
