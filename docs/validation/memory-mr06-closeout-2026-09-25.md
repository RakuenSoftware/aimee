# MR-06 closeout — 2026-09-25

MR-06 is complete against its eight unchanged acceptance clauses. MR-01 through
MR-06 are complete (6 of 18). MR-07 has not been started.

## Delivered behavior

Diagnostic retrieval now captures native lexical/cosine scores, actual RRF arm
ranks and contributions, candidate-order resets, negation and PageRank effects.
Each invocation owns its trace identity and candidate list. Bounded metadata
includes observed source revisions and selected/omitted dispositions and persists
in the existing scoped recall trace store. Shared schema 44 is additive; private
schema remains 37. Ordinary recall keeps detailed capture disabled.

Assembly distinguishes supplied-but-omitted references from assembled items across
ordinary memory, code, facts and every typed channel. These observations precede
provider preparation and do not claim delivery. Schema-2 receipts bind the exact
final provider body, source and assembly commitments, renderer, policy and owning
dispatcher. Existing schema-1 receipts remain readable. The audit owner durably
appends preparation and admission before handoff; request-write observations and
provider acknowledgements remain separate stages.

An exclusive dispatcher lifetime fence makes recovery conservative: only a former
resolved dispatcher with preparation but no admission can be classified as unsent.
Any admitted attempt lacking durable acknowledgement stays uncertain. Authorized
`aimee memory receipt <request-id> --json` reads exact-principal stored receipts
and reports independent evidence dimensions. Bounded lookup refuses partial event
sets instead of turning missing admission into a false non-dispatch claim.

Commitment-only is the default and cannot reconstruct a payload. Opt-in replayable
retention stores the exact final body in the existing encrypted principal vault,
with a 48 KiB raw-body cap, one-hour replay access and explicit removal. Expired
ciphertext is deleted lazily on lookup. Missing/deleted inputs make replay
unavailable without invalidating retained chain inclusion. `--replay` explicitly
requests payload bytes; ordinary lookup reports availability without returning them.

## Acceptance evidence

| Gate | Evidence |
|---|---|
| A1: tiny envelope and every channel | `TestIngressPackingDispositionsEveryChannel`, typed/fact repacking and native accepted-envelope tests distinguish assembly from omission; real-provider tests bind exact final bytes. |
| A2: actual dense and graph contribution | Real PostgreSQL semantic-only and PageRank diagnostic regressions assert native cosine, fusion arm and graph steps; the unchanged retrieval corpus passes both placements. |
| A3: concurrent/nested isolation | Eight concurrent trace regressions, nested identity separation, candidate and serialized bounds; full race suite passes. |
| A4: preparation without admission | Native durable admission-refusal/reopen test, recovery-state regressions, and real fork/SIGKILL exclusive-ownership test establish the ownership prerequisite for `prepared_without_dispatch`. |
| A5: post-admission uncertainty | Stage-recovery regressions cover admission without later observations; genuine after-send owner outage and host SIGKILL retain receipts and never invent acknowledgement. |
| A6: timeout after send | Native timeout/outage observations remain unresolved, including after host restart; transport errors cannot prove non-dispatch. |
| A7: changed binding | Body, source, renderer and policy mutations reject the prior binding; actual provider-byte mismatch and stale-source dispatch refusal are exercised. |
| A8: retention and inclusion | Real encrypted-vault binary round trip, principal isolation and removal; expiry and missing-input regressions preserve chain inclusion while refusing replay. |

The complete [Go/PostgreSQL race suite and standalone export](memory-mr06-final-evidence-2026-09-25/memory-race-export.txt)
pass (450.711 seconds and 5.253 seconds respectively). [All 77 repository checks](memory-mr06-final-evidence-2026-09-25/lint.txt)
pass. [Native ingress](memory-mr06-final-evidence-2026-09-25/native-ingress.txt),
[WORM ownership/crash](memory-mr06-final-evidence-2026-09-25/native-worm.txt),
[additional binding/retention regressions](memory-mr06-final-evidence-2026-09-25/binding-retention.txt)
and the [candidate CLI smoke test](memory-mr06-final-evidence-2026-09-25/cli-receipt.json) pass.

The matched [ranking benchmark](memory-mr06-final-evidence-2026-09-25/ranking-benchmark.txt)
uses 128 input records across two arms. Baseline median is 45.770 microseconds,
tracing disabled 45.242, and tracing enabled 82.473. Disabled tracing retains
61,384 bytes / 136 allocations per operation; enabled tracing uses 86,290 bytes /
497 allocations. This measures fusion capture, not database or provider latency.

| Fresh CT109 topology | Passed checks | Exit |
|---|---:|---:|
| T2: Server with optional KB | 1224 | 0 |
| T3: Server without KB | 680 | 0 |

Application: `50874e508`; test follow-up: `d7071204d`; T2 harness: `50874e508`;
T3 harness: `bbc95e548`. Both use image `sha256:28c8009bdcaca6193577ee435cd77a1a2f602ca4e5e9bd67b2193d18c47556f2`.
The [manifest](memory-mr06-final-evidence-2026-09-25/manifest.json),
[image identities](memory-mr06-final-evidence-2026-09-25/image-identities.json),
[actual process exits](memory-mr06-final-evidence-2026-09-25/process-exits.json) and
[run provenance](memory-mr06-final-evidence-2026-09-25/run-provenance.json) retain
these distinctions. All three applications used the unchanged 32 KiB operator
request ceiling. The common 18-proposal acceptance inventory is unchanged.

[Earlier failed evidence](memory-mr06-final-evidence-2026-09-25/validation-notes.json)
is retained under `initial-red`: the minimal private-store fixture needed the
owner/revision schema, a dense assertion used the wrong arm name, a test temporary
path bypassed TMPDIR, and the first T3 run failed during refresh-owner recovery.
The recovery helper could accept a read from the process it had just told to
terminate; it now waits for that process to exit before checking recovery. The
T3 rerun changes this test barrier, not application behavior or its assertions.

## Bounds and rollout

The [implementation contract](memory-mr06-implementation-2026-09-25.md) records the
bounds: 256 candidates / 128 KiB trace metadata, 12,000-byte assembly metadata,
and complete receipt reads limited to 64 events / 192 KiB detail. Hidden SQL-excluded
records are not enumerated. Source families remain explicitly unassessed when
retrieval supplies no claim-specific lineage projection. Unversioned input paths
report unavailable source coverage. Exact-payload replay does not claim decision
replay; local chain inclusion does not claim external checkpoint comparison or
provider effects. These dimensions are deliberately independent.

[All test projects were removed](memory-mr06-final-evidence-2026-09-25/cleanup.json),
including the retained T3 retry containers, networks and volumes. Production
[CT100 remains healthy on released 0.4.5](memory-mr06-final-evidence-2026-09-25/production-045-health.txt);
the [local thin client remains authorized](memory-mr06-final-evidence-2026-09-25/thinclient-production.json)
against 192.168.1.100:8743. Candidates ran only on CT109. Rollback may disable
optional diagnostics or replay retention but must preserve durable receipt stages.
