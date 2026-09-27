# Typed projection retention through outer Go packing

The host previously extracted only `rendered_context` from the KB typed response.
That discarded stable IDs and projection identity before outer packing. A typed
block that fit its own limit could exceed the space left after code, previews and
facts, causing the entire block to be omitted.

The C adapter now forwards the owner response as an opaque JSON string. This also
preserves item numbers such as int64 revisions that would otherwise pass through
cJSON doubles. The Go owner checks schema, exact bytes, projection digest,
accounting, canonical channel encoding and ordered retained references. A new
`selection_digest` binds the projection digest and retained IDs. Unknown channels,
missing/duplicate references and inconsistent bodies or commitments are rejected.
This is consistency checking of a response from the trusted owner connection,
not a cryptographic authorization signature or current-source eligibility proof.

The selection commitment hashes the version-one JSON object with fields in this
order: `schema_version`, `projection_digest`, `retained_items`. Retained rows have
`channel` then `stable_id` and preserve packing order. Encoding uses compact Go
encoding/json output, including HTML-sensitive character escaping. The published
value is `sha256:` followed by the lowercase hexadecimal digest of those bytes.

At the typed block's existing packing position, Go calculates the remaining byte
allocation, including the group header. It removes complete rows in the existing
reverse channel order and writes the retained projection once. No extra lookup
or host round trip is needed. Empty optional wrappers are omitted. The assembly
response reports source and retained projection/selection digests, exact byte
accounting, retained IDs and omitted row count. Failed retrieval is explicitly
unavailable while unrelated context remains usable; degraded retrieval stays
unknown for sufficiency. The existing C bus is unchanged.

## Local evidence

The required PostgreSQL-backed memory and family race suites pass in 52.659 and
1.409 seconds. Focused tests cover literal zero, partial and complete fits,
equal-length content mutation, changed IDs, wrong schema/accounting, missing rows,
unknown channels, exact int64 values, ambiguous inputs, empty/degraded retrieval
and owner failure. Both Server and KB host runtime placements are exercised.

The native KB adapter test verifies unchanged raw response bytes containing an
int64 revision. Native ingress and IR module-plan tests use the Go subprocess.
The ingress test shows a small assertion survives a 1,040-byte outer budget when
the complete two-row typed projection cannot fit. Existing golden envelope,
scope, query, integrity and exposure-feedback checks pass. Ownership, C boundary
and all 17 semantic-context contract/evidence checks pass.

## Remaining acceptance

The returned evidence describes Go envelope assembly before the host integrity
gate. It is not a provider release, dispatch or acknowledgement receipt. Source
versions and temporal eligibility must still be rechecked at final release;
protected content, final provider token caps, task coverage and durable receipts
remain open. This importer requires the owner's selection commitment; mixed
versions lacking it do not qualify as verified typed input. Legacy unstructured
`temporal` input remains compatible but cannot claim typed retained-ID accounting.

## Fresh deployment results

Application/harness `e88fe83a20` passed **824/824 checks** on `.253` CT 9498:
**534/534** in enrolled T2 and **290/290** in standalone T3. The authenticated
KB gate includes 24 typed-budget/identity checks, including independent selection
commitments for the original projection and each zero/partial/exact-fit cap.
Existing private/shared correction, review, rollback, restart, outage, erasure,
semantic, exploratory, identity and 160 provider-boundary checks pass.

[Named T2 verdicts](memory-shared-reliability-2026-09-20/fresh-t2-e88fe83a20.json),
[named T3 verdicts](memory-shared-reliability-2026-09-20/fresh-t3-e88fe83a20.json),
[provider accounting](memory-shared-reliability-2026-09-20/provider-accounting-e88fe83a20.json)
and [nine verified image identities](memory-shared-reliability-2026-09-20/image-identities-e88fe83a20.json)
contain no prompt bodies or credentials. Application image:
`sha256:429fa52f3d1b01a715a9e8cb2797a562ca8c785696345c03fdb17049000fdcf4`.
PostgreSQL and embedder identities match the preceding validated image run.
Raw evidence remains at `/opt/aimee-memory-proposals-evidence/t2-e88fe83a20`
and `t3-e88fe83a20`. The nine task containers were stopped after completion;
volumes and receipts remain available. Subsequent changes only record evidence
and documentation, leaving the tested application code unchanged.

Fresh provider fixtures still use caller-supplied Go recall projections; automatic
workspace ingress repacking is covered by the native C-host/Go-process tests,
not claimed as exercised by those fresh provider captures. Full MR-03/MR-06 and
MR-18 acceptance remains open. No new whole-request P95 claim is made.
