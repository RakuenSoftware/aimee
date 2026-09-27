# MR-12 served views: implementation candidate

MR-12 remains in progress pending deployed acceptance.
Named explicit views and canonical memory claim cards now use the Go memory
owner in private and KB placements. Candidate selection, eligibility, evidence,
packing and exact-byte receipts stay in Go; native adapters translate requests
and preserve owner replies. No automatic injection or optional ranking policy
is enabled.

The initial PostgreSQL/race fixtures pass for both placements, including empty
cache invalidation, new constraints and contradictions, hidden-side exclusion,
canonical correction, revocation, literal zero budgets, exact receipt hashes,
distinct historical coordinates, and private/shared isolation. Private fixtures
use the existing mutation owner after an initial direct provenance assignment
was correctly rejected. An initial shared fixture also lacked its runtime role;
that fixture setup was corrected. Earlier failed runs are not acceptance passes.

Native transport tests pass after correcting fixture call-counter ordering.
The CLI argument-spec differential suite passes all 131 shipped specifications
and 1,253 samples, including large-ID card requests. Two earlier memory receipt
and health specifications lacked samples; those coverage gaps were filled.
All 77 lint checks ran. Ownership hashes, formatting, and native MCP parity
were corrected and their targeted checks pass. The regenerated route descriptor
is included; the HEAD-based generated-doc check will run after this commit.

The full Go memory race suite passed in 399.607 seconds and the exported Go
module build passed. The first full run failed only outdated command-discovery
counts, corrected before this rerun. Native adapters and 1,253 argument samples
pass. Live CT109 upgrade/view acceptance remains. Production CT100 has not
received this candidate; its 0.4.5 server, embedder and PostgreSQL remain healthy.
