# Proposal: optimization surface residual decision points

- **State:** pending
- **Status refreshed:** 2026-06-09
- **Split:** shipped reward loop, `kb_fusion_mode`, `delegate_routing`, bandit export/replay, and optimize CLI work moved to `docs/proposals/done/optimization-surface.md`.

## Remaining Work

- Add the `briefing_style` decision point for compact versus evidence-heavy session briefings.
- Add the `guardrail_strictness` decision point for strict versus balanced guardrail thresholds.
- Define and validate any online-exploration enablement that remains default-off today.
- Benchmark-suite generalization was tracked separately and is now complete in `docs/proposals/done/memory-benchmark-suite-generalisation.md`; it was not a blocker for the shipped optimization surface.
