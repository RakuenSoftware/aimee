# MR-05 closeout — 2026-09-25

MR-05 is complete against its seven unchanged acceptance clauses. MR-01 through
MR-05 are complete (5 of 18). MR-06 was not started during this work.

## Delivered behavior

The existing Go memory owner evaluates bounded task requirements over retained,
versioned evidence. Deterministic templates cover current state, comparison,
temporal change and procedure application. Optional roles stay optional; unknown
shapes stay unknown. Requirements describe subjects, relations and answer roles,
not expected answer IDs. Confidence and retrieval count do not establish coverage.

Temporal bundles require the canonical predecessor link, the same claim, coherent
belief/valid time and a stated change anchor. An unrelated episode cannot supply
that link. Predecessor identity is withheld unless it is eligible in the caller's
scope. Procedure application needs both a reviewed procedure and its active
constraint. Source groups use MR-04's scoped lineage owner in a stable snapshot.
Independent support remains unavailable when the owner has no certificate;
copies never manufacture independent witnesses.

Both typed and outer-host packing reevaluate coverage from retained IDs and
versions. Dropped required evidence becomes `budget_dropped`; dropping a conflicting
item does not resolve the conflict. A complete diagnostic does not authorize
release: the existing canonical source fence rejects changed, revoked, hidden or
unavailable sources before dispatch.

Recovery is opt-in and requires authenticated user authority from the host's
separate command context. The host admits at most one round of scoped canonical
reads, 16 new items, 4096 tokens, 2000 milliseconds of work and zero external model
cost, intersected with task and packing limits. Serialized UTF-8 byte bounds count
items and proofs conservatively for the recovery token cap. Source-chain lookup,
reviewed-procedure lookup and counterexample-span expansion use existing owners.
Unavailable source/index contracts remain gaps; arbitrary external tool work is
not authorized by a recovery proposal.

Schema 43 adds principal-isolated, content-free recovery reservations. Reservation
commits before work, independently of the outer read transaction. Cancellation,
rollback and restart cannot reset a consumed round. Concurrent and repeated
attempts are recorded and refused; an unknown pending outcome never counts as
success. A further round requires a new host-admitted task revision. Only actual
owner reads produce outcomes and candidates for reevaluation.

## Acceptance and validation

| Gate | Evidence |
|---|---|
| A1: unrelated confidence | Current/comparison regressions, frozen incomplete fixtures and shipping comparison gate. |
| A2: packing | Typed and outer packing regressions report `budget_dropped`, including dropped temporal updates and procedure constraints. |
| A3: timeline coherence | Pure false-timeline fixtures plus real functional corrections read through canonical predecessor history. |
| A4: source revocation | Canonical-owner regression refuses the retained complete projection after real invalidation; both native deployment suites exercise source refusal and recovery. |
| A5: independent support | Thirty-copy regression and shipping uncertified-origin gate cannot claim complete independent support. |
| A6: bounded recovery | Real PostgreSQL token/item/latency caps, rollback/restart reservation persistence, eight concurrent attempts admitting one round, authenticated admission and shipping durable duplicate refusal. |
| A7: separate evaluation | Frozen corpus reports false-complete **0/9** on intentionally incomplete cases and deterministic retained-object reader accuracy **6/11**. These are separate metrics; model answer quality is not measured by this deterministic gate. |

The frozen corpus is unchanged from its implementation commit; the common
18-proposal acceptance inventory is also unchanged. Their hashes are recorded in
the [manifest](memory-mr05-final-evidence-2026-09-25/manifest.json). Gold answer objects
remain in the evaluator and are never supplied to the serving requirement planner.

The complete [memory race suite and standalone export](memory-mr05-final-evidence-2026-09-25/memory-race-export.txt)
and [all 77 repository checks](memory-mr05-final-evidence-2026-09-25/lint.txt) pass.
The module descriptor owns the new code, tests and frozen fixture; standalone
export exercises the MR-05 templates, canonical owner and recovery tests.

| Fresh CT109 topology | Passed checks | Exit |
|---|---:|---:|
| T2: Server with optional KB | 1221 | 0 |
| T3: Server without KB | 677 | 0 |

Application: `b1e707c83`; T2 harness: `52cd371b0`; T3 harness: `b1e707c83`; immutable image `sha256:b86c7c9c64bf5aadcf44b87165d5f47025cd94cdace998c089a16c60da2fa2ba`.
[Image identities](memory-mr05-final-evidence-2026-09-25/image-identities.json),
[actual process exits](memory-mr05-final-evidence-2026-09-25/process-exits.json),
all fixture JSON and [cleanup verification](memory-mr05-final-evidence-2026-09-25/cleanup.json)
are retained. [All MR-05 test stacks, networks and volumes were removed](memory-mr05-final-evidence-2026-09-25/all-test-stack-cleanup.json). All three application containers used the unchanged 32 KiB operator
request ceiling. Production CT100 remains healthy on released 0.4.5; the local
0.4.5 thin client remains paired with 192.168.1.100:8743. Candidates ran only on CT109.

The [run provenance](memory-mr05-final-evidence-2026-09-25/run-provenance.json)
identifies the separate passing T2 and T3 processes. Earlier failed receipts are
retained under `initial-red`, `storage-failure` and `episode-fixture-red`. These
cover the corrected host-admission transport/error checks, CT109 disk exhaustion,
and a race between a synthetic indexer-owned episode and the actual indexer.
The T2-only fixture fix holds the producer's parent-row barrier during observation
checks; it changes no serving assertion. The application and T3 harness are unchanged.

The [implementation contract](memory-mr05-implementation-2026-09-25.md) describes the
opt-in API and conservative rollout boundaries in more detail. Recovery can be
disabled without restoring the old nonempty-to-complete shortcut.
