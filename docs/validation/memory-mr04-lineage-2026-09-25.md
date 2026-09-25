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
