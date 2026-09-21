# Typed assertion source-version commitments

Typed assertion selections now retain the exact shared owner ID, assertion ID and
assertion revision observed by their selection query. The `semantic_assertion`
record kind separates that namespace from memory-row identities. Decimal strings
preserve exact IDs and revisions through the external C host. The owner UUID is
read in the same SQL snapshot as the assertion, using an uncorrelated scalar
subquery; this adds no database or module round trip.

`retained_items[].source_version` is included in the existing selection digest.
Outer packing preserves it for retained rows and removes it for omitted rows.
A changed owner or revision cannot reuse the prior selection commitment. The Go
decoder rejects a version that contradicts the exact assertion ID/revision in
selected bytes, even if the supplied metadata digest has been recomputed. Duplicate
selection detection remains keyed by channel/ID, independent of source metadata.
Unversioned earlier projections retain their prior digest encoding.

`source_version_state` distinguishes `record_versions_observed`, `partial` and
`unavailable`. These states describe selected record-version evidence only. They
do not certify producer authentication, supporting-parent versions, current
eligibility or final release. Channels without owner revision contracts do not
receive invented versions. An empty retained selection reports unavailable.
The model-facing projection bytes are unchanged; version metadata stays outside
the prompt. No whole-request latency improvement is claimed.

Local validation:

- Full Go/PostgreSQL memory suite passes, including the restricted runtime role.
- Targeted race tests cover real owner identity capture through the public Go
  command, exact large IDs, owner replacement, revision/ID tampering, inconsistent
  metadata, unchanged and zero-byte outer packing, and unversioned mixed channels.
- Standalone Aimee export/build and all 17 S1 contract checks pass. The module
  descriptor includes the new Go source and tests.
- All 77 lint checks, native build and memory routing pass. Fresh authenticated
  KB execution remains pending. The fresh harness adds five checks for observed versions, digest binding,
  stable replay, revision changes and omission. Earlier 1,553-check images do
  not contain this implementation.

Remaining work includes episode/summary parent and collection versions, version
contracts for learning-owned observations/procedures, and owner revalidation at
final release. This is not completion of MR-01, MR-03, MR-04 or MR-06.
