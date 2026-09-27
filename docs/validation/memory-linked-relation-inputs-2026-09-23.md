# Linked relation input versions — 2026-09-23

The relation generator copied a linked memory's content while retaining only the
main parent identity. Relation search, entity edges and entity profiles could
therefore return that copy after the linked memory expired, changed, retired or
moved scope. The [before replay](memory-linked-relation-inputs-2026-09-23/before.txt)
reproduces the expired-target leak under the packaged PostgreSQL runtime role.

The generator now captures each input's ID and record revision from the same
row read that supplied its text. Those observations commit with generated rows
in the existing lineage ledger. Repeated identical derivations union their inputs.
Link generation applies current validity and suppression in addition to its
existing source/target scope rule. The three serving surfaces check all observed
inputs for exact revisions, visibility and current eligibility before limits or
profile aggregation. A lifecycle restore cannot certify an old copy at a newer
source revision.

Generator-owned legacy rows without observations await canonical reindexing.
Authored relations retain their existing parent gate. No schema or C-bus change
is required. Reindexing refreshes observations transactionally; the existing
late-failure fixture continues to check rollback of the whole rebuild.

The full memory PostgreSQL/race suite passes in 139.020 seconds. The independently
exported owner also builds and passes its regressions. Memory ownership, C boundary,
module bus and descriptor checks pass. [Race](memory-linked-relation-inputs-2026-09-23/go-race.txt)
and [export](memory-linked-relation-inputs-2026-09-23/export.txt) logs are retained.
The first broad run required updating two minimal test schemas with the existing
lineage table, record revision and restricted-role SELECT grant; the
[initial fixture result](memory-linked-relation-inputs-2026-09-23/fixture-update-required.txt)
is retained separately from the passing final run. Fresh-image validation is recorded below.

The regression covers target expiry, suppression,
retirement, content and scope changes, restore without reindex, legacy missing
observations, authored rows, and exclusion of expired inputs during generation.

This closes a concrete direct copied-input gap in MR-01/MR-04. General transitive
lineage, automatic dependent requeueing, full erasure closure and provider release
receipts remain open. Invalidated copies are withheld until canonical rebuilding;
this change does not claim that all dependants rebuild automatically.

A follow-up also records the originating link ID with each linked input. Serving
requires that link to still connect the recorded source and target with the same
relation. Deleting or retargeting the link cannot leave an apparently current
relationship merely because both memory revisions stayed unchanged. The internal
observation marker advances to version 2; version-1 rows await reindexing. The
restricted-role fixture adds link deletion and relation-edit cases. Fifteen new
HTTP checks per KB topology exercise copied-source expiry, restore without a new
observation, fresh observations and link deletion across graph search, entity
edges and profile aggregation. The full PostgreSQL/race suite passes in 139.042
seconds after batching observation inserts once per parent. A two-origin fixture
also proves that identical relation text retains both input observations and
withholds the copy when either origin expires. [Final race](memory-linked-relation-inputs-2026-09-23/batched-input-race.txt)
and [export](memory-linked-relation-inputs-2026-09-23/batched-export.txt) checks pass.
Fresh application/harness `09aa330ea` passes **1,140/1,140 checks**: 125 in
T1 and 1,015 in T2, including all 30 linked-input HTTP checks. The
[raw receipts](memory-linked-relation-inputs-2026-09-23/fresh/T1/topology.json)
and [T2 receipts](memory-linked-relation-inputs-2026-09-23/fresh/T2/topology.json)
retain individual verdicts. All nine running container image identities were
[captured](memory-linked-relation-inputs-2026-09-23/fresh/image-identities.json),
including the actual 32,768-byte provider cap on all three application containers.
The application image is `sha256:3287db9b28123de7445d4ae3124c876049d98c15dae4545ca7be444e04b8ee0c`;
the database uses schema-32 image `aimee-pr2990-postgres:686b99ea0` and the embedder
uses released 0.4.5. Both harness processes exited zero and removed their containers.
The task-owned PostgreSQL replay fixture remains for continuing implementation.
These functional parallel runs are not matched performance measurements and do
not validate the later durable consumer or execution-policy changes.
