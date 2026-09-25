# MR-04 final closeout — 2026-09-25

MR-04 is complete against its eight unchanged acceptance clauses. MR-01 through
MR-04 are complete (4 of 18). MR-05 is next; it was not started during this closeout.

## Implemented result

The Go memory owner exposes scoped claim lineage with host-recorded creation
families, bounded traversal and conservative support diagnostics. Thirty copies
retain the same origin; A+B does not mint a third witness. Missing, cyclic,
unversioned or truncated inputs remain incomplete. Different files, URLs or
creation events do not establish independence. The shipping storage projection
has no independence-certification producer and reports an unknown independent
count; the pure projection verifies explicit certificates when supplied. No
support-based ranking or confidence formula is enabled.

Cognification, deterministic indexing, episode cards, folding and legacy style
inference record actual producer observations. Readers check required ancestors,
versions, audience and validity before release. Episode-card selection also
checks new session inputs; style inference pins the rules collection. Generated
rules reach native consumers through the Go owner. Historical outputs without
observations remain unavailable as current guidance. The [producer inventory](../proposals/pending/memory-reliability-04-producer-inventory.md)
records each managed output's dependency and erasure contract.

The existing dependency registry follows transitive inputs and remains diagnostic;
it cannot authorize release of stale text. Deterministic indexing refreshes only
the summary observations it rebuilt, avoiding repeated whole-graph reconciliation.
Native context refresh replaces prior native proofs once per context build,
accumulates every native block in that build and preserves other retained-context
proofs. Composed recall also pins the private collection before selecting inputs.
Cache invalidation rejects fills started before the invalidation.

Durable consumer positions survive restart, duplicate delivery and rollback;
retention gaps force a fresh canonical snapshot. Subject erasure captures required
registered owners, including offline owners, and completes only after authenticated
durable acknowledgements. Private session/delegation intents reject late writes
and replay before either private reader starts. Shared retained intent rejects
old IDs and matching erased payloads. Restoring snapshots requires preserved
control metadata and stopped serving until replay succeeds. Completion covers
managed application stores; detached exports/backups and external provider copies
are explicitly outside verified deletion coverage.

## Acceptance evidence

| Gate | Verification |
|---|---|
| A1: copies | `TestEvidenceLineageCopiesAndComposites`, scoped PostgreSQL projection and shipping HTTP thirty-copy gate preserve one origin without new votes. |
| A2: composites and unknowns | Composite, claim-boundary, certificate and shared-fixture tests preserve root sets and unknown independence. |
| A3: incomplete ancestry | Missing, cycle, depth, node-bound, version and undeclared-input regressions remain explicitly incomplete. |
| A4: revocation | Transitive current-release and registry tests; rule/style/card source changes; native refreshed-context and cache/owner refusal process checks. |
| A5: crash/replay/erasure | Durable consumer restart, duplicate delivery, failed queue transaction and retention-gap tests; offline-owner receipt and native coordinator regressions. |
| A6: collection dependencies | Shared/private empty-view tests invalidate after new input; style rules-collection and episode session-input regressions. |
| A7: scoped lineage | Restricted-role tests and shipping hidden-versus-missing diagnostic equality withhold parent identities and counts. |
| A8: restoration | Shared content/identity intent and late-write tests, restricted private snapshot-replay tests, startup barriers and shipping restore rejection. |

The [complete memory race suite and exported owner](memory-mr04-final-evidence-2026-09-25/memory-race-export.txt)
pass with the PostgreSQL replay database enabled. [All 77 repository checks](memory-mr04-final-evidence-2026-09-25/lint.txt)
pass. The [private-store family race suite](memory-mr04-final-evidence-2026-09-25/private-family-race.txt)
also passes on the final code. The focused [composed collection and multi-block
proof regressions](memory-mr04-final-evidence-2026-09-25/composed-collection.txt) and
[native context refusal test](memory-mr04-final-evidence-2026-09-25/native-context-refusal.txt)
pass. The native ownership ledger was refreshed after reviewing the transport-only
proof-accumulation flag; selection and release remain with Go. Earlier component
receipts for the startup barrier
and native coordinator/cache are retained in the
[implementation checkpoints](memory-mr04-lineage-2026-09-25.md).

| Fresh CT109 topology | Passed checks | Exit |
|---|---:|---:|
| T2: Server with optional KB | 1214 | 0 |
| T3: Server without KB | 677 | 0 |

Application and both process harnesses: `22f75f247`. Both process runs use the
same immutable image:
`sha256:3a108a42fa1d4cb1de4034d23f4c42c83dfa8578ac9e2d7d34aed5b884369e58`. [Image identities](memory-mr04-final-evidence-2026-09-25/image-identities.json),
[actual exits](memory-mr04-final-evidence-2026-09-25/process-exits.json), all per-fixture
JSON and [cleanup verification](memory-mr04-final-evidence-2026-09-25/cleanup.json)
are retained. The [manifest](memory-mr04-final-evidence-2026-09-25/manifest.json)
records the unchanged acceptance-inventory hash and schema versions (shared 42,
private 37; release policy `current-validity-v17`).

The earlier indexing timeout, native refresh failure, asynchronous-indexing race, minimal-scope fixture errors
and routing-fixture collision remain as red receipts beside their passing
regressions. The routing fixture now reserves fresh colliding identities above
both sequences instead of adopting a previously revoked KB row. This changes
fixture isolation, not release policy. Native scenarios also wait for prior runs'
asynchronous feedback indexing to settle before capturing their initial shared
proofs; deliberate new shared constraints and private corrections still refuse
stale retries before a second provider send. Failed runs are not counted as passing runs.
Production CT100 was checked separately: server, database and embedder remain
healthy on released 0.4.5. The local 0.4.5 thin client remains paired with
192.168.1.100:8743. Candidate code was validated only on CT109.
