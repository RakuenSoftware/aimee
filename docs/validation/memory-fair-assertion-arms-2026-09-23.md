# Fair semantic-assertion candidates — 2026-09-23

The semantic-assertion owner filled its result array with lexical hits and then
refused every new vector candidate once that array reached the requested limit.
Graph expansion used the same full-array gate. The existing RRF step therefore
never saw independent candidates when lexical retrieval filled the pool.

The owner now unions the bounded lexical and vector pools before applying the
final top-k. Graph expansion gets a separate quota of at most the requested
limit, deduplicated anchors and at most min(64, 2 × limit) lookups across its
existing maximum of two hops. Every lookup retains the same current/historical,
scope, parent-visibility and lifecycle predicates. The existing vector threshold,
RRF60 arithmetic, authority tie-break and stable-ID tie-break remain unchanged.
Evidence hydration runs only for final retained results.

Graph-only hits now receive a graph-arm rank. The lexical search on a graph anchor
is no longer incorrectly counted as a lexical match to the original query. The
response identifies `assertion-arm-union-v1`, the union's candidate count, graph
work and budget exhaustion. An overlapping assertion whose owner/revision changed
between collection statements refuses the request rather than combining versions.
These observations are bounded candidate evidence, not a complete universe trace.

A restricted-runtime-role PostgreSQL fixture reproduces the old full-lexical
failure. Under the fixed owner, a dense-only candidate competes and survives
fusion; a graph-only candidate also survives, without a fabricated lexical vote.
A hidden supporting parent still excludes the graph assertion. The full memory
PostgreSQL suite passes in 56.664 seconds. The
[before result](memory-fair-assertion-arms-2026-09-23/before.txt) and
[Go result](memory-fair-assertion-arms-2026-09-23/go-tests.txt) are retained.

This advances MR-09 candidate admission and MR-06 tracing. Joint prior caps,
independent-origin diversity, exposure controls and held-out quality/performance
gates remain open. It does not certify either proposal.

The fresh-deployment harness also now requires a nonempty operator byte ceiling
(default 32768 bytes) and verifies that the actual application container received
it. Without this fixture setting, 73 operator-ceiling tests per Server topology
were silently skipped. This is test configuration, not a production policy change.
Fresh validation of this candidate is pending.

The full PostgreSQL/race suite passes in 117.451 seconds; the actual native
Go-owner ingress fixture and memory ownership/C/descriptor checks also pass.
[Race](memory-fair-assertion-arms-2026-09-23/race-tests.txt) and
[native](memory-fair-assertion-arms-2026-09-23/native-tests.txt) results are retained.
