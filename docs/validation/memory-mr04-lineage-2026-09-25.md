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
| Deterministic relations and episodes | Versioned producer observations | Extend ownership coverage to cognified relations |
| Cognified soft rules | Global-scope and protected-rule safeguards | Bind generated rule revisions and all rule consumers to their inputs |
| Session folding | Legacy lineage; originals deleted by compaction | Retain verifiable origins without losing the successful compaction contract |
| Derived artifact registry | Existing dependency inventory and rederivation queue | Versioned transitive closure and complete producer coverage |
| Relation invalidation consumer | Durable progress, replay, retention-gap resnapshot | Coverage for other derived owners |
| Query-derived views | Scoped collection generations and release foundations | Explicit empty-view/new-contradiction acceptance proof |
| Subject erasure | DB1/DB2 erasure protocol | Verified retained-copy coverage for every required owner, including offline owners |
| Restoration | Rejection tombstones | Independent durable deletion intent replay before serving restored snapshots |

MR-04 remains the only active proposal. These checkpoints do not certify any of
the eight frozen gates as complete.
