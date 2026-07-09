# Implementation plan: sliced-lifecycle "build" workflow

Slices are ordered by dependency and are individually shippable. Each slice is itself
delivered as a PR that goes through the roundtable and merges into this effort's feature
branch — mirroring the lifecycle we are building. Live panel + live forge remain
integration-gated; their logic is mock-tested in-slice.

## Slice S1 — Multi-input roundtable executor (pure; mock-tested)
- Teach `exec_roundtable` (`src/workflow/wfe_roundtable.c`) to walk `node->ins[]`, resolve each
  binding to its producer's stored artifact content, and assemble a composite review packet
  instead of the single `row.content_hash`.
- Add an optional `focus`/`review_lens` param (free-form string) surfaced to the panel prompt so
  the acceptance gate can be framed as "completion + code quality + missing tests."
- Tests: multi-input packet assembly, single-input back-compat (existing `build.yaml`/`hotfix`/
  `managed-change` behavior unchanged), focus param plumbed. Uses the existing mock panel.
- Unblocked. No DB/forge/graph changes.

## Slice S2 — Live panel provider wiring (§0 posture; logic mock-tested)
- Register `wfe_set_panel_provider` in `wfe_autonomy_register` (`src/server/wfe_live_delegate.c`)
  with a provider that convenes the ensemble roundtable (`delegate_ensemble` /
  `roundtable_pipeline`), passing S1's composite packet + required/eligible personas and returning
  per-persona verdicts.
- Fail-closed preserved: panel that can't compose → DEGRADED park (never a silent single-lens pass).
- Tests: verdict mapping, eligibility/quorum, fail-closed on unreachable panel (mock ensemble).
- Depends on S1's composite-packet shape.

## Slice S3 — Feature branch + PR base targeting
- New `branch.open` block: create/return a durable feature branch → `WFE_ART_BRANCH` (append-only
  catalog + validator entry; version-stability test).
- Add a `base:` binding/param to `pr.open` (and `merge`) so a PR targets the feature branch
  (sub-PRs) or the default branch (final PR). Extend the forge seam (`wfe_forge_t.open`/`merge`)
  to accept an explicit base; keep the protected-branch guard (final PR to default stays
  human-gated, no autonomous merge to `main`/`master`/`release*`).
- Tests: catalog/validator typing, base resolution, protected-branch refusal, forge base plumbing
  (mock forge).

## Slice S4 — Sub-workflow-per-slice capability (the load-bearing slice)
- DB1: parent↔child work-item linkage (child carries `parent_id`, `packet`, target feature branch).
- New `foreach.workflow` block: for each packet from `split`, instantiate a child work item running
  the `slice` workflow; parent returns `WFE_STEP_PENDING` until all children are terminal.
- Aggregate predicate + driver: advance the parent only when all children merged; any parked/failed
  child parks the parent for a human. Reuse the existing autonomy driver to advance children.
- Tests: fan-out N packets, park-until-children, all-merged → advance, one-parked → park, zero-packet
  edge case. Mock-driven (no live delegate/forge).
- May sub-slice into S4a (DB1 + linkage) and S4b (block + driver + aggregation) if the PR grows large.

## Slice S5 — Workflow YAMLs + validator/version tests
- Author the parent `build` workflow and the child `slice` workflow per the proposal's target
  lifecycle; wire the acceptance gate's `focus` lens and the multi-input plan/proposal gate.
- Retire/replace the old `config/workflows/build.yaml` (keep `managed-change`/`hotfix`/`manual-review`).
- Tests: `aimee workflow validate` clean for both; canonical/version stable; a "no merge to protected
  branch without a human gate" assertion; an "every pr.open is followed by gate.ci before merge"
  assertion.

## Slice S6 — End-to-end glue + drivable run
- Register the panel provider (S2) at init, confirm the acceptance lens (S1), final human gate +
  green-CI + merge-to-default (S3/S5) compose, and add an integration-gated end-to-end drive
  (mock providers) that takes a work item from proposal to merged feature PR.
- Tests: end-to-end happy path (mock), non-convergent slice parks, non-green final CI blocks merge.

## Sequencing
S1 → S2, S1 → S5; S3 → S5; S4 → S5; S5 → S6. S1, S3, S4 (DB1 part) are independently startable.
Live panel (S2) and live forge (S3) wiring are integration-gated; everything else is mock-tested and
unblocked today.

## Cross-cutting
- **Append-only enums** for any new block/artifact; a version-stability test guards stored hashes.
- **Fail-closed** is asserted per gate (panel degraded → park; non-green CI → block; protected base →
  refuse).
- **Delegation:** each slice is delegated via `aimee delegate <role> --persona <name>` per repo
  convention, with the mechanical verify gate (`aimee git verify`) run before each slice PR.
