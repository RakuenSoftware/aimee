# MR-04 lineage implementation checkpoint — 2026-09-25

MR-04 remains open. MR-01 through MR-03 remain complete (3 of 18).
This checkpoint does not certify the eight acceptance gates.

## Reproduced release defect

A three-generation derivative remained current after its original input was
revoked. The [red PostgreSQL regression](memory-mr04-lineage-2026-09-25/transitive-red.txt)
exercises the actual shared eligibility predicate under a non-owner role.

The versioned ancestry predicate now checks every observed input under the
caller's scope and one statement snapshot. A cycle, a missing or changed input,
a hidden ancestor, invalid owner binding, missing producer observation, or an
exhausted traversal bound refuses eligibility. Existing episode-card observations
participate in the traversal. Cognified claims record both the source and derived
record revisions while their source remains locked. Older unversioned copied
claims cannot silently become independently authored records.

The [focused race regressions](memory-mr04-lineage-2026-09-25/lineage-target.txt)
pass. The first complete owner run failed after 590.632 seconds on an episode-card
regression: the new gate did not recognize card observations as covering their
legacy memory links. That check is corrected; the archive and lineage regression
subset passes. The complete suite and process validation remain pending.

## Independent support evaluator

The bounded Go evaluator distinguishes visible supporting records, established
origin families, and verified independent witnesses. Thirty copies plus their
summaries do not add an independent vote; an A+B composite preserves both roots
without becoming a third witness. Claim-specific evidence edges contribute;
related, corrective and contradictory links do not become support automatically.

Unknown origins remain unknown. Distinct URLs or ingestion identities do not
establish independence. Certificates are comparable only within their verified
set; known common dependencies collapse witness groups. Cycles, missing inputs,
changed versions and truncated traversal remain explicit. Scoped storage/public
integration and host-generated origin registration remain pending.

## Producer and coverage audit

| Existing owner/path | Observed implementation | Work still required |
|---|---|---|
| Memory cognification | Claims, relation text and global soft guidance | Claim observations added; relation/guidance observations and full producer regressions pending |
| Memory deterministic indexing | Versioned units, summaries, episodes and copied relation inputs | Integrate lineage projection and verify transitive release across all channels |
| Episode cards | Versioned input set and output digest | Ancestry traversal added; cross-producer process validation pending |
| Relation invalidation consumer | Durable positions, replay and resnapshot behavior | Extend coverage inventory to every required producer/consumer |
| Query-derived context | Collection/source versions and final release checks | Verify insertion invalidates earlier empty views, with durable progress evidence |
| Subject erasure orchestration | DB2 begin and DB1 completion acknowledgement | Complete required-owner coverage and restoration-resistant deletion intent |

Production CT100 server/database/embedder were verified healthy on released
0.4.5 at 45 hours uptime. Candidate work remains isolated from that installation.


## Scoped evidence endpoint checkpoint

The Go owner now projects a bounded, repeatable-read lineage snapshot for
`memory.evidence`. The native HTTP bridge exposes `POST /v1/memory/evidence`;
the served CLI manifest and compiled fallback both expose
`aimee memory evidence <id> --json`. Store selection preserves personal memory
by default and requires explicit `--store kb` for shared memory. IDs travel as
exact decimal strings. The bridge transports owner diagnostics without deriving
support counts in C.

Creation-event families are diagnostic identities, not independence certificates.
The storage projection therefore leaves independent support unknown. It also
reports generation unavailable until generation-bound lineage is implemented.
Missing, inaccessible, unsupported and oversized dependencies withhold counts and
references. A missing ancestor and an unauthorized ancestor produce identical
responses. Existing pure projection tests exercise verified independence rules;
they do not establish producer-side certification in the deployed store.

Focused PostgreSQL/race validation passed in 1.140 seconds. Native owner-envelope
routing passed, including exact IDs above JavaScript's safe integer range and
malformed-owner rejection. CLI argument parity passed across 128 served specs
and 1,250 differential samples. The local native build required GNU C17 and the
existing PostgreSQL development libraries. The first full Go race run failed its discovery-count assertion and then timed
out at 600.024 seconds in the direct hard-rule source fixture. The discovery
expectations now include the new command; the hard-rule fixture now explicitly
disables JIT, matching production handleData transaction settings. The focused
discovery/rule/lineage rerun passed in 1.450 seconds; the public evidence envelope
rerun passed in 1.112 seconds. A complete race/export rerun remains pending; no fresh-image or acceptance-closeout claim is made.

## Remaining producer and owner audit

| Producer/owner | Existing binding | Remaining MR-04 work |
|---|---|---|
| Cognified memories | Versioned direct inputs and transitive release predicate | Claim-specific origin projection and generation binding |
| Episode cards | Versioned input set, payload digest and transitive release predicate | Collection dependency and end-to-end revoked-cache proof |
| Deterministic relations and episodes | Versioned producer observations | Cognified relations now bind exact inputs; legacy unowned relations remain unclassified |
| Cognified soft rules | Global-scope and protected-rule safeguards | Bind generated rule revisions and all rule consumers to their inputs |
| Session folding | Legacy lineage; originals deleted by compaction | Retain verifiable origins without losing the successful compaction contract |
| Derived artifact registry | Existing dependency inventory and rederivation queue | Versioned transitive closure and complete producer coverage |
| Relation invalidation consumer | Durable progress, replay, retention-gap resnapshot | Coverage for other derived owners |
| Query-derived views | Scoped collection generations and release foundations | Explicit empty-view/new-contradiction acceptance proof |
| Subject erasure | DB1/DB2 erasure protocol | Verified retained-copy coverage for every required owner, including offline owners |
| Restoration | Rejection tombstones | Independent durable deletion intent replay before serving restored snapshots |

MR-04 remains the only active proposal. These checkpoints do not certify any of
the eight frozen gates as complete.


## Cognified relation checkpoint

Cognification now registers its own relation ownership separately from the
replacement index. It captures source and output revisions in the same locked
producer transaction. Existing authored/indexed collisions retain their owner.
The shared current/historical relation predicate requires observations for both
known producers; cognification observations also verify owner and output revision.
The serving policy identifier advances to `current-validity-v13`.

The restricted-role PostgreSQL regression first reproduced a missing-observation
leak (one eligible relation when zero was expected). The repaired predicate passes
missing observation, wrong owner, changed source/output, hidden input, revoked
input and revoked ancestor cases. Re-extraction replaces the old observation
without duplicating the relation. A separate packaged-schema replay exercises
the actual cognification writer and restricted runtime grants (1.947 seconds).

The complete race/export rerun uses `PGOPTIONS=-c jit=off` so direct
SQL fixtures use the same planner setting as production memory requests. Legacy
relations without producer ownership, generated soft rules, folding, registry
integration and the other unresolved gates remain open.

The first relation full-suite run finished in 223.859 seconds and failed only
`TestDomainPublicPostgres` and `TestRuntimePublicPostgres`: their minimal relation
fixtures lacked the already-deployed `record_revision` column. Both fixtures now
include it. The combined domain/runtime/producer rerun passed in 2.717 seconds;
the complete rerun passed: memory race suite 224.272 seconds, exported-owner build
check 5.414 seconds.


## Empty native recall observations

Native shared and private recall now observe their memory collection before
selecting rows. Even an empty or zero-text projection carries that observation
through the existing source-release protocol. Shared observations bind the
canonical effective audience and the sum of its visible monotonic collection
heads; private observations remain local. Revision one represents generation
zero. Owner and audience changes cannot reuse a coincidentally equal head.

Restricted-role PostgreSQL tests exercise the actual recall producer and release
check. A visible inserted constraint invalidates the earlier empty view without
changing a selected parent. A hidden collection insert leaves the full scoped
observation byte-identical. A different audience with an equal numeric head is
rejected. The private producer similarly detects a new private constraint.
Focused collection/native/activation/private/release tests passed in 3.357 seconds;
a second private/owner-routing selection passed in 1.793 seconds. The broader
runtime replay and complete-suite validation remain pending for this component.

This binds native recall's memory collections. It does not certify collection
coverage for every typed assertion, learning, reminder, directive, export or task
producer; those remain part of the MR-04 producer inventory. The heads are durable
control metadata and must survive content-only restoration.

The first broader collection replay failed after 321.300 seconds when graph
feedback exceeded the runtime fixture's deadline. Its evidence-parent check now
reuses the same materialized eligible-memory set already used for node admission,
instead of recomputing recursive ancestry for each parent. This preserves the
same statement snapshot and hidden/missing-parent refusal. The rerun is pending.

The cached-text regression retains only the actual selected descendant reference
(without collection metadata), revokes its grandparent, and checks release refusal
while the selected descendant's text and revision stay unchanged. Activation
recall now uses the same transitive ancestry predicate before selecting rows;
the regression checks its before/after behavior too. Ordinary pending commitments
retain their existing selection contract.

Production remains released 0.4.5: server, PostgreSQL and embedder were healthy
at 46 hours; the paired local thinclient reached /v1/health with HTTP 200.

The collection/runtime replay passed in 186.010 seconds after the graph-feedback
change. A new ordinary-recall regression then reproduced one returned descendant
after its ancestor was revoked. Ordinary recall now uses the transitive gate too,
and validity diagnostics report `derived_inputs_unavailable`. The focused rerun
passed. The serving policy advances to `current-validity-v14`; full-suite
validation is pending.

Complete validation for this component passed: the memory race suite took
225.336 seconds and the exported-owner build check took 5.400 seconds. Module
ownership, Go boundary, source registration, test registration, documentation
and proposal links also passed. The production 0.4.5 installation is unchanged.
Time-only applicability changes and non-memory collection coverage remain open.

## Collection validity boundaries

A future-dated constraint can become applicable without a database write. The
regression reproduced the earlier collection-only check accepting a view across
that boundary. Collection observations now also carry the earliest future valid
time boundary in the visible collection. Release checks, including the end of
the five-second send lease, must precede it. Private collections similarly bind
the earliest expiry. Hidden rows cannot affect the exposed deadline.

Focused PostgreSQL/race tests passed in 1.988 seconds, including both owner
placements, hidden-boundary noninterference, syntax rejection and send-window
expiry. The complete rerun is pending. This does not yet cover eligibility
changes caused solely by lineage/auxiliary-table mutations or every non-memory
collection; those remain open producer-coverage work.

The temporal-boundary complete rerun passed: memory race suite 257.407 seconds
and the exported-owner build check passed. Module ownership, source/test
registration, documentation and proposal-link checks passed. Raw results are
retained in `collection-time-full-race-export.txt`.
