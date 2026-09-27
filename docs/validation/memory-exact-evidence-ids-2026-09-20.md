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
CI then caught a PostgreSQL-only test query: `LIKE` required an explicit cast
from `jsonb` to text. The test now uses the portable cast; fidelity, demotion and
ranker-fit all pass against fresh PostgreSQL clones of an owned template. This
follow-up changes the test query and evidence only, not the tested application.
The codec packaging CI job and lint pass on `7bb36b1551`; full CI remains pending
the corrected test commit.

Application/harness `7bb36b1551` passes **853/853** fresh checks on `.253` CT 9498:
**563/563** in enrolled T2 and **290/290** in standalone T3. The 18 new authenticated
checks cover exact IDs in all event projections, provenance and outcome requests,
malformed/overflow/unsafe numeric rejection, embedded-NUL rejection, and absence
of partial events after refusal. Existing review, revision, erasure, concurrency,
restart, rollback, outage, semantic and provider-boundary checks continue to pass.

[Named T2 verdicts](memory-shared-reliability-2026-09-20/fresh-t2-7bb36b1551.json),
[named T3 verdicts](memory-shared-reliability-2026-09-20/fresh-t3-7bb36b1551.json),
[provider accounting](memory-shared-reliability-2026-09-20/provider-accounting-7bb36b1551.json)
and [all nine image identities](memory-shared-reliability-2026-09-20/image-identities-7bb36b1551.json)
contain only named verdicts, image identities and count/digest provenance.
Application image:
`sha256:06504a0d32da12f39c27171a8fff590394713e23f689d12a0de7cfc8868205fd`.
PostgreSQL and embedder identities match the preceding validated run. Raw receipts
remain under `/opt/aimee-memory-proposals-evidence/t2-7bb36b1551` and
`t3-7bb36b1551`. All nine tested containers are stopped; their volumes and
evidence are retained. The first intermediate image `41428a7a13` also passed both
fresh deployments before the final caller and packaging fixes.

These fixtures prove identity transport; high-ID provenance targets need not
exist and do not certify canonical source-version binding. The native client
regression separately exercises automatic evidence/outcome request serialization.
The codec does not reconstruct identities already lost in legacy floating-point
storage. Complete MR-06 all-channel durable receipts and dispatch/crash uncertainty
remain open. No new whole-request P95 improvement is claimed.
