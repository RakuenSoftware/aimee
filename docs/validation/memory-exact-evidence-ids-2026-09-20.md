# Exact source identity in retrieval evidence

Evidence transport must preserve the memory owner's signed 64-bit identity.
The external C host and artifact adapters previously converted ordinary memory
IDs through cJSON doubles in request encoding, canonical references, derived
projections, merge deduplication and provenance lookups. Adjacent IDs above the
JSON safe-integer range could become the same source. Attribution payloads and
the corresponding outcome handlers had the same defect.

The adapters now retain numeric JSON for safe integers and use canonical decimal
strings outside that range. Readers accept both representations, refuse unsafe
numeric values and malformed/overflow strings, and never synthesize an identity
from already ambiguous legacy data. A failed legacy merge leaves its stored
payload unchanged; provenance marks an undecodable source as unavailable.
Evidence/outcome HTTP actions reject embedded NULs before cJSON can truncate a
string to a valid identity prefix. The matching KB document ranker capture and
training reader use the same codec. Ranker string subject keys remain supported.
The Go memory owner, selection policy and C bus are unchanged.

Local validation covers signed boundaries, adjacent large IDs, exact event
projections and merge retries, unchanged ambiguous legacy payloads, attribution
storage, native client requests, fidelity storage and distinct ranker feature
joins. All five native test binaries pass: json-fluent, demotion, fidelity,
kb-client-memory and ranker-fit. The 71 link-closure, 14 declaration-ledger and 22 source-boundary
regressions also pass. Memory ownership and the C-boundary check pass.

The prior CI failures were stale DB2 metadata after the bounded trace-read fix.
The declaration and link-closure manifests have been regenerated and reviewed.
The link closure retains 140 system dependencies and zero project-owned debt;
no acceptance threshold was relaxed. The shared codec header is explicitly declared as an export input,
so the isolated repository can compile the same JSON wire contract without
importing application helper APIs or crossing a module boundary. Its exported core and DB2 executable build successfully on `.253`.

The caller audit also repaired automatic memory/ranker outcome and ranker-event
requests. Native exact-ID reads now send exact strings and preserve large integer
response tokens before cJSON converts them to doubles. Overflow responses fail
instead of returning a fabricated ID. The native client regression covers all
four evidence/outcome requests and exact-ID reads through int64 maximum.
The Go/PostgreSQL memory and Aimee-family race suites pass (52.661s/1.445s).
The 17 semantic-context harness tests also pass.

Fresh deployment results will be recorded after the new application image runs
through both owned T2/T3 environments on `.253`. These identity fixes do not
complete MR-06: canonical source-version binding, all-channel durable receipts,
and dispatch/crash uncertainty remain open. No new whole-request P95 claim is
made.
