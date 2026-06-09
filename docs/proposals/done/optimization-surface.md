# Proposal: optimization surface shipped work

- **State:** done
- **Status refreshed:** 2026-06-09
- **Split from:** `docs/proposals/pending/optimization-surface.md`

## Shipped

The main optimization-surface work has landed.

- The reward loop is closed: `kb_bandit_reward()` reaches `db2_bandit_decision_close()`.
- `kb_fusion_mode` is a registered sampled decision point and is closed with a recall-sufficiency reward.
- `delegate_routing` is a registered sampled decision point and is closed after delegate execution.
- The phantom-export issue is fixed by data-driven bandit export payloads.
- Typed `/v1/intelligence/bandit/export` and `/v1/intelligence/bandit/replay-record` surfaces exist.
- `aimee optimize points|baseline|replay|run|compare|promote` is shipped.

## Verification Notes

Verified in-tree evidence: `src/kb/kb_bandit.c`, `src/kb/kb.c`, `src/server/server_compute.c`, `src/kb/kb_bandit_registry.c`, `src/kb/http/kb_http.c`, and `src/cmd_optimize.c`.
