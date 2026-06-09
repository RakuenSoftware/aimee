# Proposal: optimization surface residual decision points

- **State:** done
- **Completed:** 2026-06-09
- **Split from:** `docs/proposals/done/optimization-surface.md`

## Shipped

The residual decision points are now represented in the reviewed optimizer
registry and exposed through the existing `aimee optimize`/export surface.

- `briefing_style` selects compact versus evidence-heavy session briefing
  bundles. The promoted arm is honored by `memory.briefing`; compact caps the
  default budget while evidence-heavy raises it and over-fetches more rows before
  truncation.
- `guardrail_strictness` selects balanced versus strict semantic guardrail
  thresholds. The promoted `strict` arm lowers warn/prompt/block thresholds while
  preserving the existing dry-run/advisory gates.
- Online exploration remains default-off through
  `intelligence.bandit.live_decision_enabled`. These two residual points are
  marked `static`: operators can promote arms from replay or manual evidence,
  and live sampling should not be enabled for them until an automatic reward loop
  is validated.
- Benchmark-suite generalization remains complete separately in
  `docs/proposals/done/memory-benchmark-suite-generalisation.md`.

## Verification Notes

Verified in-tree evidence: `src/kb/kb_bandit_registry.c`,
`src/memory_advanced.c`, `src/guardrails_orchestrator.c`,
`src/tests/test_bandit.c`, `src/tests/test_memory_advanced.c`,
`docs/COMMANDS.md`, `MANUAL.md`, and `api/openapi-server-v1.yaml`.
