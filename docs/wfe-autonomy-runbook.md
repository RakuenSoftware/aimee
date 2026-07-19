# Runbook: autonomous WFE lifecycle

The autonomous workflow engine (WFE) drives a proposal from intake to a mergeable pull request entirely server-side: a work item is created per proposal, durable per-work-item state tracks each gate, and the primary agent manages while delegates implement bounded slices in isolated worktrees, with every transition audited and gated before the next stage runs. fix-validate-1784468081.

- proposal intake
- plan
- roundtable gate
- implement per-slice
- verify
- PR open