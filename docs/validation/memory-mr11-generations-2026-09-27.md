# MR-11 embedding generations: functional closeout

Functional implementation and acceptance complete on `ba8f0a631`. This change adds
full embedding identity commitments and versioned background indexing to the Go
memory owner. It does not enable the MR-07, MR-09, or MR-10 optional policies.

## Implemented behavior

The embedder health protocol publishes a complete, immutable model/tokenizer and
preprocessing identity alongside its legacy alias. Go verifies the commitment
and rejects malformed or mismatched complete identities without falling back to
the alias. Actual vector dimensions must also match the full identity at
indexing and query time; preparation rejects an incompatible storage dimension.
A provider that supplies only an alias remains `legacy_unknown`.
Legacy aliases remain available to older provider readers; those readers do not
gain a full-identity guarantee from this update.

Shared whole-memory/unit generations bind exact canonical revisions and input
hashes. Retained semantic assertions have their own generation storage and
background queue; recall embeds only the query. Canonical assertion and evidence
changes invalidate derived assertion vectors in every retained generation.
Schema-owned cleanup triggers also work when invoked by the subject-erasure
owner, without granting that owner general index writes. Their fixed search path
resolves canonical relations before caller temporary relations; a PostgreSQL
regression proves cleanup cannot be redirected to a temporary shadow table.

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
- Full-identity dimension regression: passed, 1.786 seconds; exported owner rebuilt.
- Temporary-table shadow and erasure regression: passed, 1.395 seconds.
- Python identity commitment and Go family migration unit checks passed.

## Deployed acceptance

The immutable `aimee-pr2990:ba8f0a631` image built with exit zero. Its image ID is
`sha256:40f0521f2c0c81ee74e894934a3509129719357109d1af5ff443f65bfc2597dd`.
Both existing isolated CT109 owners upgraded with their identities and volumes
preserved and became healthy. The native upgrade audit passed 12 checks with
exit zero: the persisted `db1` owner reached migration 40, personal/code owners
reached version 2, background private indexing published the exact source
revision, native search worked, retirement withheld current reads and explicit
fixture destruction cascaded all retained vectors. Optional health, selection
and horizon policies remained unset on both owners.

A fresh disposable KB using that image passed 17 checks with exit zero. It
exercised two generation cutovers, retained historical assertion semantics,
clock-only future activation without a canonical version change, rollback,
retirement, physical fixture erasure, restart recovery and readiness reporting.
Changing only the provider query prefix at dimension 384 produced lexical
fallback with no old-generation semantic trace. Backfilling and activating the
new commitment restored semantic retrieval; reverting provider and generation
restored it again. Every assertion query reported zero indexed documents.

The provider fixture used the pinned 0.4.5 model weights and installed runtime,
with the new health protocol. Its prefix variant changed only the query prefix.
This does not relabel production's legacy provider identity as verified.
The disposable stack and its volumes were removed after the run.

Earlier harness failures are retained rather than counted as passes: missing
provider cap/operator authority and undrained indexing during fixture startup;
then a duplicate canonical assertion triple, an incorrect string scope for the
assertion-search API (v7), and treating model-authority retirement as physical
erasure (v8). The final v9 fixture uses valid distinct triples, `include_all`
for assertion search, and an exact-fixture owner destruction after retirement.
No production authorization rule was weakened to satisfy these checks.

## Acceptance mapping and limits

| Frozen gate | Evidence |
|---|---|
| A1, A7: identity separation and query-time checks | Full identity/dimension regressions; deployed same-dimension prefix swap, new cutover and rollback |
| A2: resumable work | Private/code interrupted bounded-batch regressions reuse committed rows; shared reembed replay does not regenerate completed drafts; deployed restart retains active semantic reads |
| A3, A9: edits, erasure, revocation and rollback | Shared reembed replay rejects changed sources and suppressed drafts; code deletion/rollback and private offline-provider revocation tests; deployed all-generation fixture destruction |
| A4, A5: historical and future semantic coverage | Deployed superseded assertion at explicit valid/belief time and future assertion crossing its boundary without a write |
| A6: horizon and retained admission | Full suite includes MR-10 current-versus-history/horizon tests; generation admission excludes processing-prohibited inputs independently of current utility/validity; retained private versions survive expiry and rebuild |
| A8: bounded read work | PostgreSQL rebuild-lock test returns lexical fallback within one second without aborting its transaction; deployed assertion queries perform no backlog refresh |

The advertised modes are those listed above: retained assertion semantic history
is covered; private memory/code and whole-memory/unit adapters do not claim a
historical semantic API. Request-level readiness remains conservative. Synthetic
fixtures do not establish production task quality or promote optional policies.
Family migration 40 and owner schema version 2 do not promise old-binary startup;
validated rollback here means compatible vector-generation rollback.

CT100 stayed healthy on `aimee-native-core:0.4.5-bridge.2`, with healthy 0.4.5
embedder and PostgreSQL containers. This candidate was not deployed to CT100.

[Checks, frozen harnesses and hashes](memory-mr11-evidence-2026-09-27/README.md)
