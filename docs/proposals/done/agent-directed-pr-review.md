# Proposal: Agent-directed PR review

- **State:** done
- **Completed:** 2026-06-09
- **Author:** JBailes
- **Date:** 2026-06-09
- **Charter roles:** Review, Reason

## Shipped work

This proposal is complete. The review-mode roundtable can now be invoked by an
agent with a caller-supplied brief and a diff, and it returns structured review
state rather than only consolidated prose.

Implemented behavior:

- `roundtable_opts_t` accepts an optional `brief`, and review prompts carry that
  direction without suppressing out-of-brief blocking findings.
- Review runs retain structured `items`, `answered_questions`, and
  `coverage_gaps` in `roundtable_result_t`.
- `POST /v1/delegate/roundtable` serializes the structured result fields.
- The MCP surface exposes `ensemble_review {brief,diff,rounds,turns}` and gates
  dispatch on the delegate capability path.
- The MCP tool queues through the async roundtable route so long-running review
  work remains cancellable and pollable through the normal run lifecycle.
- The golden MCP tool list, server dispatch tests, OpenAPI contract, and
  delegate-ensemble tests cover the new surface.

## Verification evidence

- `src/headers/delegate_ensemble.h`
- `src/server/delegate_ensemble.c`
- `src/server/server_compute.c`
- `src/server/server_compute_roundtable.inc`
- `src/server/server_mcp.c`
- `src/mcp_tools.c`
- `src/tests/test_delegate_ensemble.c`
- `src/tests/test_server_dispatch.c`
- `src/tests/test_mcp_tools_golden.inc`
- `api/openapi-server-v1.yaml`
