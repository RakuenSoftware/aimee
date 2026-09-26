# MR-11 embedding generations: implementation candidate

MR-11 remains in progress until deployed acceptance is recorded. This change adds
full embedding identity commitments and versioned background indexing to the Go
memory owner. It does not enable the MR-07, MR-09, or MR-10 optional policies.

## Implemented behavior

The embedder health protocol publishes a complete, immutable model/tokenizer and
preprocessing identity alongside its legacy alias. Go verifies the commitment
and rejects malformed or mismatched complete identities without falling back to
the alias. A provider that supplies only an alias remains `legacy_unknown`.
Legacy aliases remain available to older provider readers; those readers do not
gain a full-identity guarantee from this update.

Shared whole-memory/unit generations bind exact canonical revisions and input
hashes. Retained semantic assertions have their own generation storage and
background queue; recall embeds only the query. Canonical assertion and evidence
changes invalidate derived assertion vectors in every retained generation.
Schema-owned cleanup triggers also work when invoked by the subject-erasure
owner, without granting that owner general index writes.

Private memory generations retain independent historical revisions. Private code
generations bind the published project watermark and exact file inputs. Both
workers stage at most sixteen documents per batch, preserve completed work across
restart, recheck inputs before writing, and activate only complete generations.
Cutover validates dimensions, nonzero vectors, hashes, and admitted source
versions. Rollback validates current admission and applies intervening deletion
and revocation. Private revocation cleanup precedes model access, including when
the model is offline.

Code reads join the active validated project generation and keep lexical fallback
usable after optional vector SQL failure. Private memory reads report the pinned
generation and current/validated watermarks. Shared maintenance status reports
class coverage, queue state, watermarks and storage readiness. Shared request
readiness remains conservatively `lagging` when scoped completeness has not been
proved; successful candidate retrieval is not represented as complete coverage.
A busy shared rebuild lock immediately falls back instead of consuming the
request's deadline.

Coverage is explicit: whole-memory/unit and private code adapters serve current
semantic requests; retained assertion generations support current, historical,
valid-time and belief-time selection. Private retained memory vectors do not
advertise a historical semantic API. Other KB vector owners are not relabeled as
verified by the Go memory adapter.

## Local validation

- Complete memory race suite: passed, 350.809 seconds, after correcting two
  erasure-owner permission failures in the first run (358.771 seconds).
- Exported memory process: passed, 5.329 seconds, after removing duplicate test
  entries from the descriptor.
- Repository lint: all 77 checks passed. The earlier run found a missing SQLite
  shape-only table mirror; the mirror adds no SQLite vector-serving behavior.
- Focused code generation acceptance: passed, including interrupted rebuild,
  restart reuse, deletion before rollback, generation-aware query and SQL fallback.
- Focused erasure/private cleanup/identity/rebuild checks: passed, 2.146 seconds.
- Final vector-validation predicate regression: passed, 1.749 seconds.
- Python identity commitment and Go family migration unit checks passed.

## Deployment acceptance still required

Build the immutable candidate, migrate the isolated CT109 owners, and exercise
retained/future semantic coverage, restart, generation rollback, and cleanup over
the deployed interfaces. Record actual process exits and verify production
CT100 remains healthy on 0.4.5. Family migration 40 and owner schema version 2
require explicit binary compatibility assessment; this report does not promise
that older binaries can start after those migrations.

[Evidence directory](memory-mr11-evidence-2026-09-27/)
