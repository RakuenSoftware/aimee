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
Fresh image results will be recorded against the candidate revision.
