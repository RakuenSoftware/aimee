# Versioned fact query batching

The plain-text fact projection previously read each entity separately. Besides
repeating SQL/store-bus calls, that could observe selected assertions and parents
from different statement snapshots. Go now reads the ordered entity candidates
through one lateral query, with the existing 32-fact limit per entity. All returned
assertions and direct parent revisions share that statement snapshot. Entity-name
discovery remains a separate candidate step; this is not a collection-freshness
or final release guarantee.

The common Go formatter owns PII/confidence filtering and line-size policy for
both compatibility and versioned recall. Batching preserves user-first/entity
ordering, confidence/ID ordering, complete lines, and the legacy trailing-NUL
capacity rule. An overflowing line stops that entity while a smaller later
entity can still use the remaining allocation. Parent overflow and query/scan
failures refuse the projection rather than returning partial version evidence.

A [matched benchmark](memory-fact-query-batching-2026-09-21/benchmark.json) compares
actual implementations at `f055d212f5` and the new batching code, using the same
benchmark fixture in isolated worktrees. The transaction-owned fixture has 36
assertions across the user and eight mentioned entities, 36 memory parents,
a 2,048-byte fact allocation, and a non-owner role with parent RLS.
Three one-second runs measure:

| Metric | Before | Batched |
|---|---:|---:|
| SQL/store calls per recall | 11 | 3 |
| Median time | 2.93 ms | 2.45 ms |
| Median allocated bytes | 207,271 | 127,438 |

That is a 16.2% reduction in local query time and 38.5% fewer allocated bytes.
[Before](memory-fact-query-batching-2026-09-21/before.txt) and
[after](memory-fact-query-batching-2026-09-21/after.txt) retain raw samples. This
is not whole-request/provider P95 and does not certify the complete MR-18 matrix.

The full Go/PostgreSQL suite and all 77 lint gates pass. Runtime-role parity
checks compare ordinary and versioned recall over six byte capacities, including
a smaller later entity after an overflowing first entity. Existing source-version,
parent eligibility, public command, packing and exact-ID regressions remain.
Targeted runtime-role race, native build/routing, real C-host/Go-process ingress,
standalone export, 17 S1 and documentation/link checks pass. Fresh application/harness `430cb91fd373395c2235f38b3da2efd9ae60be72` passes
**1,570/1,570 checks**: [978 T2](memory-fact-query-batching-2026-09-21/fresh-t2-430cb91fd3.json)
and [592 T3](memory-fact-query-batching-2026-09-21/fresh-t3-430cb91fd3.json).
The application image is `sha256:c5d15bcd6b153caa2bf135da1342105a794542b5b917c226d86cd94d48205c97`.
All [nine image identities](memory-fact-query-batching-2026-09-21/image-identities-430cb91fd3.json)
and three actual 32 KiB provider caps were checked. All nine owned containers
and nine empty networks were removed, preserving images, volumes and raw receipts.
CI run 35585859589 reports a failing T1 whole-projection stability assertion;
this local/fresh result does not claim that CI passed.

The memory owner and module-side bus client remain Go; the C bus is unchanged.
