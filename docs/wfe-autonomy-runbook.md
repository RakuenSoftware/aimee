# WFE Autonomy Runbook

The WFE (Work Flow Engine) autonomy lifecycle moves a work item from a raw operator or upstream signal all the way to a merged change on the forge without manual hand-offs between stages. Each stage is owned by a deterministic tool, emits a structured artifact, and gates the next stage so that the pipeline can be re-driven from any intermediate artifact if a later stage fails or is re-run.

- **Proposal intake** — the WFE captures a new work item, attaches scope metadata, and opens a worktree branch for it.
- **Plan** — the WFE produces an approved plan with acceptance criteria that downstream stages verify against.
- **Roundtable gate** — the plan is reviewed and either accepted, rejected, or sent back for revision before any code changes are made.
- **Implement per-slice** — each accepted unit of work is delegated, edited, and committed on the work-item branch in isolation.
- **Verify** — every slice is checked with the project's verification commands and any failures are fixed before the slice is accepted.
- **PR open** — the accepted work is pushed and a pull request is opened against the base branch for human review.

final-e2e-1784471710