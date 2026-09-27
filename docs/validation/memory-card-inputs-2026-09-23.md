# Generated episode-card input observations — 2026-09-23

Generated episode cards now record their collection owner, canonical parent
revision, unit-field digest and every selected source revision in the existing
lineage table. Source rows remain share-locked through model generation and
atomic publication. Source selection remains bounded to 200 non-card canonical
inputs. No schema migration is introduced.

The `current-validity-v9` predicate requires those observations when a canonical
record contains a generated `episode_card` unit. Current retrieval and indexing
withhold the card when an input changed, expired, became suppressed, was revoked
or is no longer visible to the caller. Historical inspection permits retained
inputs under its existing lifecycle policy, while requiring exact observations
and refusing hidden or revoked inputs. Missing observations do not make an old
generated card independently authored. Custom authored card types retain their
existing policy. An existing stale generated card is refused rather than
returned as a successful new generation.

Public PostgreSQL fixtures exercise source edits, revocation, expiry, hidden
scope, changed unit text, missing observations and canonical card edits. Both
card listing and canonical direct reads withhold affected cards. The packaged
runtime replay passes in 183.155 seconds after separating its preserved legacy
card fixture from the ordinary source used for unit embedding. The first full
race run exposed that fixture coupling; it was not a passing run.

Bounded public database requests disable PostgreSQL JIT for their transaction
only. Nested source checks otherwise incurred compilation cost disproportionate
to the small request. Fixture indexes now match the production identity probes,
and public-domain fixtures use the real request transaction settings. Pending
commitment recall retains its pending lifecycle contract while adding the card
input check.

The final card-only full race suite passed in 230.311 seconds, and its export
build passed in 4.309 seconds. Fresh deployment validation of the card change
remains pending. This bounded
card-input repair does not implement arbitrary transitive closure, collection
query dependencies, automatic regeneration, independent source families or
restore-proof erasure, and does not complete MR-01 or MR-04.
