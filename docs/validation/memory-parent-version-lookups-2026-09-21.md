# Direct memory-parent version commitments and lookup performance

Typed assertion projections retain their direct live memory parents' exact owner,
record ID and revision from the same SQL statement as the selected assertion.
`memory_parent_state=observed` distinguishes an observed empty parent set from old
metadata whose parent state is unavailable. A parent content change changes the
selection digest even when rendered assertion bytes are identical. Parent IDs
are canonical, sorted, unique and bound to the assertion owner. Outer packing
preserves the existing selection commitment contract.

The typed query observes at most 65 distinct parents: the extra row detects the
64-parent capacity limit and makes the channel unavailable instead of certifying
an incomplete list. Ordinary assertion search does not incur parent collection
or this limit. This is direct memory evidence only, not a claim that episode,
non-memory or transitive dependencies are complete, or that final release has
been reauthorized. Those remain required work.

The shared eligibility predicate now parses canonical memory locators on the
evidence side and probes the memory primary key through a bounded lateral join.
It avoids scanning all visible memories under row-level security. Canonical
locator equivalence tests cover bigint boundaries, malformed strings, overflow,
leading zeroes and signs. The existing deny-any-ineligible-parent behavior and
live/all-evidence distinction are preserved for each caller, including hybrid
graph expansion and fact recall.

A PostgreSQL microbenchmark with 10,000 memories, 16 evidence parents and a
non-owner role enforcing project RLS measures:

| Query | Three-sample range | Median |
| --- | --- | --- |
| Previous concatenated parent ID | 2.30–2.70 ms | 2.45 ms |
| Parsed locator with ordinary join | 1.87–1.90 ms | 1.90 ms |
| Production bounded primary-key lookup | 50.5–50.9 µs | 50.7 µs |

The median query ratio is **48.3×**; all variants report 344 client bytes and five
allocations per operation. These are local query measurements, not whole-request
P95, production latency or MR-18 acceptance. [Raw samples](memory-parent-version-lookups-2026-09-21/lookup-benchmark.txt)
and [fixture, hashes and calculations](memory-parent-version-lookups-2026-09-21/lookup-benchmark.json)
identify the benchmark and production predicate.

Local PostgreSQL and runtime-role tests exercise parent revision changes,
unchanged rendered content, parent overflow refusal and unchanged ordinary search.
Projection tests reject contradictory observation states and preserve legacy
unknown state. Full memory tests, targeted race tests, native build/routing,
standalone export and 17 S1 checks pass. All 77 lint gates and documentation/link
checks pass. Fresh deployment of `5aee043dd6` passes as recorded below.

The preceding `91a1f02969` CI exposed a missing `creation_retry_test.go` ownership
entry; the descriptor is fixed and all 47 descriptor tests pass. Its T2 gate also
reported a restored-episode miss and a generic deletion failure. The episode
fixture reused its parent's generated episode key: background indexing replaces
that episode and changes the latest row returned by `EpisodeGet`. A runtime-role
regression confirms the parent-key replacement and preservation of a distinct
curated episode key. The E2E fixture now uses separate parent and authored episode
keys. The deletion failure was subsequently reproduced on indexed parents and repaired
in [schema 29](memory-evidence-event-isolation-2026-09-21.md). This change retains failed
MCP responses and exports only fixed
memory failure categories/SQLSTATEs from logs, excluding SQL, request content,
connection strings and driver messages. Classification covers the production
store-bus `StoreError`, native PostgreSQL errors, unavailable store transport,
closed transactions and result-capacity refusals; unit/race tests verify that
wrapped private messages cannot enter the diagnostic output. No retries or eligibility relaxations
were added. Fresh validation of this change must not be represented as a proven
repair of the later-reproduced deletion failure.


Fresh application/harness `5aee043dd63f672c7b22f9f2c63717ef6c09af89` passes
**1,560/1,560 checks**: **968 enrolled T2**, **592 standalone T3**. This includes
all seven owner/assertion/parent-version checks and unchanged provider/native gates.

- [T2 checks and receipts](memory-parent-version-lookups-2026-09-21/fresh-t2-5aee043dd6.json)
- [T3 checks and receipts](memory-parent-version-lookups-2026-09-21/fresh-t3-5aee043dd6.json)
- [Images, actual caps and cleanup](memory-parent-version-lookups-2026-09-21/image-identities-5aee043dd6.json)

Application image: `sha256:6ffe172a35b856fb75ba9f3797288400fe85003485ecf651a1d621e25a23414c`. All nine image identities
and three actual 32 KiB caps were verified. Nine owned containers and nine empty
networks were removed; images, volumes and raw receipts remain. Expected injected
failures are retained as diagnostic categories, separately from named verdicts.
These images precede the authored-episode fixture repair, store-wire diagnostic
improvements and schema-29 evidence-event repair. The latter is covered separately
in [event isolation](memory-evidence-event-isolation-2026-09-21.md); the passing
matrix above did not deterministically index the deletion fixture beforehand.
