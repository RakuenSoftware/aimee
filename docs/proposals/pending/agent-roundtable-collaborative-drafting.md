# Proposal: Agent roundtable residual follow-up

- **State:** pending
- **Status refreshed:** 2026-06-09
- **Split:** shipped engine, participant-routing, temperature, config, CLI route, and `/v1/delegate/*` work moved to `docs/proposals/done/agent-roundtable-collaborative-drafting.md`.

## Remaining Work

- Add deeper behavioral tests for multi-round convergence, scoring, failure handling, participant compatibility, and sequential turn order.
- Add or tighten MCP exposure if the roundtable should be callable as a first-class MCP tool, not only through the CLI route surface.
- Validate runtime economics for large panels: budget preflight, per-round deadline enforcement, and degraded-result metadata under partial participant failure.
- Confirm user-facing docs/help describe aggregate versus roundtable behavior after the shipped route and engine changes.
