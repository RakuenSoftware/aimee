# WFE Autonomy Runbook

A work item (WI) flows through the `aimee/wi` pipeline from a submitted proposal to a merged PR without operator intervention: the daemon drives intake, planning, gating, implementation, verification, and delivery as a sequence of stages, each producing a durable artifact and a `state` transition that the next stage consumes. Operators use this document to orient themselves before inspecting a live WI.

## Pipeline stages

- **Proposal intake** — the WI enters the daemon's queue with a scope file and acceptance criteria.
- **Plan** — the plan delegate decomposes the scope into ordered units and records them on the WI.
- **Roundtable gate** — the roundtable delegate reviews the plan; approval unblocks implementation.
- **Implement (per-slice)** — the code delegate executes each unit on its session branch and commits it.
- **Verify** — `aimee git verify` runs after each unit; red results block the next commit.
- **PR open** — once every unit is green, the WI opens a pull request against the base branch.

## Where to look

Inspect live state of a WI via the work-item events endpoint, which streams every `state` transition, commit SHA, and verifier result emitted by the pipeline.
