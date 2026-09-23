# Episode-card source eligibility — 2026-09-23

Episode-card generation previously selected active records without checking
suppression or valid-time applicability. The Go source query now uses the shared
current-eligibility predicate before its source limit, audience compatibility
check and external model invocation. It excludes future, expired, suppressed,
superseded, archived, quarantined, deleted and revoked inputs.

The [public archive/card PostgreSQL race regression](memory-card-source-eligibility-2026-09-23/targeted-race.txt)
passes in 1.522 seconds. It checks the actual model input, verifies that a set
containing only ineligible sources never invokes the model, and confirms that a
current source still produces a card. Existing scope conflicts, idempotency,
capacity limits and rollback coverage continue to pass.

This repairs source selection at generation. It does not establish complete
versioned lineage or revalidate previously generated cards after later source
changes. Those MR-04 requirements remain open. The [export build](memory-card-source-eligibility-2026-09-23/export.txt),
ownership and documentation checks pass. Fresh-image validation is pending;
the released CT100 deployment remains on 0.4.5.
