# Recall record revisions — 2026-09-23

Ordinary private/shared recall and shared activation selection now return the
observed owner, record ID and revision alongside each selected memory record.
The payload and version come from the same PostgreSQL statement snapshot.
Identifiers and revisions in the version contract are decimal strings, including
values above JavaScript's exact integer range. Missing or invalid owner/version
metadata refuses the read instead of inventing a version.

Pending commitments retain their observed revision too. This does not change
pending records into active records or claim they satisfy ordinary current-memory
eligibility. Graph-fused records retain their existing version evidence; this
change does not attach a fresh version to an older fused payload.

Regression coverage checks private recall before and after an actual correction,
shared ordinary and activation reads after a correction with revision
`9007199254740993`, and the existing full PostgreSQL memory suite. Validation
passes: the [full PostgreSQL race suite](memory-recall-versions-2026-09-23/full-race.txt)
completed in 127.942 seconds, the [exported owner build](memory-recall-versions-2026-09-23/export.txt)
passed, and ownership, C/bus boundaries, descriptors and documentation guards passed.

This is selection evidence, not provider release authorization. Native projection
retained-source commitments, private-owner release revalidation, mixed-owner
routing and remaining channel coverage are still required. No proposal is marked
complete by this change. CT100 continues to run released 0.4.5.
