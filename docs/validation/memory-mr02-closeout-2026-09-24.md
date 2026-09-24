# MR-02 final closeout — 2026-09-24

MR-02 is complete against its eight unchanged acceptance gates. MR-01 and MR-02
are now complete (2 of 18); MR-03 is next. The original 123-clause acceptance
inventory is unchanged.

## Result

Shared and private writes preserve mutation authority, protected kinds, retained
history and exact-draft review decisions. Expected-version corrections serialize
under the canonical row lock. Actor-scoped idempotency receipts, canonical changes,
audit, history and invalidation commit together. Scoped exact key/content tombstones
reject repeated material; they do not claim paraphrase detection.

Automatic maintenance, compatibility lifecycle writers and folding preserve
user/unknown-origin and protected episode/experience/instruction/policy records.
Shared schema 36 also blocks removing a protected kind before an in-place edit.
Bulk import preserves that kind without accepting imported actor claims. Import
and export forward the host verifier's scope independently of their request body.
Filtered export now runs through the Go owner; C performs transport only. Entity
metadata is rebuilt from exported parents; `card_json` is `{}` because the global
profile cache lacks scoped source observations.

Canonical journals and scoped generations are transactional. The relation consumer
atomically queues invalidation and checkpoints bounded replay, recovering from
restart, owner changes and retention gaps. Query/release decisions independently
check canonical root and parent versions; lagging jobs or old local progress cannot
certify freshness. Durable provider protection lasts through explicit completion.
Canonical commit receipts do not claim that every derivative has caught up.

## Final verification

Application: `f716c1a80`. Authority, common and parity harnesses used that commit;
scoped credentials and T3 used `aa87ca198`, which moves the mutating scoped fixture
after its existing fixed-count assertions. T2 used `863ae9f14`, allowing its deliberately
paused-owner refresh case 180 seconds to refuse instead of 90, with unchanged
refusal/no-sixth-send assertions and explicit timeout evidence. The initial T2 run
exceeded that fixture window; its partial result is retained in component evidence.
The successful rerun completed that outage case in 69.88 seconds; it verifies the
behavior but does not establish the cause of the earlier timing variation.

One immutable application image:
`sha256:df4cde0d4bf0cf2b0a01a0fe04db66b2a842c2f55c7e48e4d72de520ddd1033a`. All six process drivers exited zero:

| Process fixture | Passed checks |
|---|---:|
| authority | 45 |
| scope | 36 |
| common | 201 |
| parity | 58 |
| T2 | 1,097 |
| T3 | 617 |
| Total | 2,054 |

[Process exits](memory-mr02-final-evidence-2026-09-24/process-exits.json), the six
fixture directories, [nine image identities](memory-mr02-final-evidence-2026-09-24/image-identities.json)
and all three actual 32,768-byte provider caps are retained. The
[cleanup receipt](memory-mr02-final-evidence-2026-09-24/cleanup.json) verifies that
fixture-owned containers and networks were removed.

The complete PostgreSQL memory race suite passed in 235.243 seconds and exported
owner verification in 6.214 seconds
([output](memory-mr02-final-evidence-2026-09-24/race-export.txt)).
The [native import/export transport tests](memory-mr02-final-evidence-2026-09-24/native-context-export.txt)
and [all 77 lint gates](memory-mr02-final-evidence-2026-09-24/lint.txt) passed on
`f716c1a80`. Subsequent fixture-only edits passed Python compilation; final proposal
links, documentation checks and whitespace checks also passed.
The race/export run tested `fb5cf0b06`; `f716c1a80` adds C verified-context forwarding,
its native tests, the scoped process fixture and generated declaration bookkeeping.
Its Go owner is identical. Shared schema upgrade and reapply both passed; private
append-only migrations were unchanged. Earlier reproductions and repairs are
recorded in the [component evidence](memory-mr02-authority-2026-09-24.md).

The [eight-gate checklist](../proposals/pending/memory-reliability-02-closeout.md)
maps each clause to implementation and tests. These are local/isolated validation
results, not a claim of GitHub CI completion or completion of later proposals.

## Released installation

All draft image/schema work ran on CT109. CT100's released server, database and
embedder remain [healthy on 0.4.5](memory-mr02-final-evidence-2026-09-24/production-045-health.txt)
after the 0.4.4 upgrade. The paired local thinclient successfully pushed the work
through the registered Aimee CLI. PreToolUse remains temporarily disabled as requested.
