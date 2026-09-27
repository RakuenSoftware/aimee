# MR-15 procedure experience and task costs — 2026-09-27

State: functional acceptance complete on candidate `27f0ac8fd`; attribution and
cost reports remain observe-only. The [evidence manifest](memory-mr15-evidence-2026-09-27/sha256.json)
pins the retained checks, harnesses, image identities and upgrade snapshot hashes.

C and Go retrieval paths no longer reward nonempty search results or dynamically
sample count-trained retrieval weights. Historical artifacts remain intact, and
explicit operator retrieval limits remain available. The learning owner admits
immutable receipt/version/attempt-bound observations, separates trial outcomes
from terminal tasks, preserves conflicts and unknowns, and reports sample counts
and uncertainty. Memory renders bounded learning-owned experience and revalidates
its revision at final provider admission. Economizer accounts for all declared
stages and retries using exact integer amounts, distinguishes actual/estimated/
unpriced entries, and requires complete frozen pairing for comparisons.

The [operator contract](../procedure-experience.md) specifies the interfaces and
limits. Public admission supports explicitly attributed user feedback. Automatic
host-execution attribution has no adapter and remains unavailable. Model claims
cannot become verified application outcomes. Declared costs and quality are not
independently verified completeness or billing evidence; no new routing, ranking
or exploration policy is promoted by these reports.

## Validation

- Full PostgreSQL memory race suite passed in 371.346 seconds; exported Go memory
  process build passed in 5.836 seconds. This includes result-count non-reward,
  restricted-role replay, bounded experience, stale experience rejection and the
  concurrent provider-send barrier.
- Learning race tests, including production function grants under a real
  restricted runtime role, passed. Economizer race tests cover exact arithmetic,
  integer rate estimates, duplicate/conflicting charges, unpriced calls and
  frozen paired denominators.
- Native learning HTTP, CLI argument, KB memory client and bandit tests passed.
  The complete KB HTTP route/TLS suite passed with its required Go fixtures.
- All 77 lint checks ran. Missing process-stage registration and a CLI file-size
  violation were fixed; their checks passed. Subsequent scope/authentication,
  ownership, schema, declaration, API and route checks also passed.
- The [live acceptance run](memory-mr15-evidence-2026-09-27/mr15-experience-27f0ac8fd-checks.json)
  passed all 28 checks through the actual C/Go owners, authenticated HTTP,
  compiled CLI and controlled provider transport. Three final requests retained
  the exact reviewed procedure and matching durable receipt. Tests cover
  duplicates, conflicting evidence, omitted/unused procedures, model authority,
  repair attempts, canonical version changes, exact amounts above 2^53,
  unknown pricing and refusal of missing paired tasks.

The first live attempt retained and delivered the procedure but refused feedback:
the writer incorrectly expected a certificate in a context that represents the
verified service identity. A dedicated governed-feedback action now requires the
existing mTLS service/caller membership intersection; the legacy application
recording route keeps its original contract. The failed run is retained in the
manifest. Broader route tests also exposed stale expectations for the old reward
name and for a PostgreSQL-backed identity lookup in a shim fixture; corrected
expectations preserve refusal and the final complete suite passes.

These are controlled functional checks, not live-model task-quality or cost-saving
measurements. Synthetic procedure and application rows were removed by exact
fixture IDs; existing stores, enrollment and model configuration were preserved.

## Upgrade

CT109 Server and KB upgraded with private database snapshots from the MR-14
candidate through `ae7b046a8` to `27f0ac8fd`. Both final owners are healthy on
`0.4.5-pr2990.27f0ac8fd`, image
`sha256:12914f82027719499c8cb897b5859903c1e872b41b07436cede4fe3752c68c7e`.
Snapshot dumps stay private on CT109; only their hashes are published. The
existing snapshot-restoring rollback controller remains available. This successful
MR-15 rollout did not exercise rollback.

Post-acceptance checks confirm that optional memory health, selection and horizon
flags remain unset. CT100 remains healthy on `aimee-native-core:0.4.5-bridge.2`
with released 0.4.5 embedder and PostgreSQL images; no candidate was deployed there.
