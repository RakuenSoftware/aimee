# Risk register

Risks use likelihood and impact from 1–5; score is their product. Scores 15–25 require immediate
treatment and release review, 8–14 require a dated plan, and 1–7 may be accepted by the owner.

| ID | Risk | Owner | L | I | Treatment | Due/status |
| --- | --- | --- | ---: | ---: | --- | --- |
| R-001 | managed server Docker socket grants host-equivalent authority | Platform Security | 3 | 5 | prefer split stack; restrict managed deployment | open; deployment decision |
| R-002 | legacy `v1-partial` WORM rows lack full-field binding | Audit | 2 | 4 | label on export; migrate new writes to v2; external witness | treating |
| R-003 | operator-supplied native modules share daemon trust | Runtime Security | 3 | 4 | approval, digest pinning, process isolation roadmap | open |
| R-004 | built-in subject erasure/retention coverage is incomplete | Data & Privacy | 4 | 4 | prohibit regulated ingestion or enforce DB lifecycle externally; implement command/reaper | release review |
| R-005 | independent reviewer capacity is not source-verifiable | Engineering Governance | 3 | 4 | branch rules, backup owner, quarterly export | open; external evidence required |

Review monthly and after incidents, material architecture changes, new processors, or changed legal
requirements. Closed risks remain in history with decision, approver, date, and evidence.
