# Historical audit recovery and repository backfill — 2026-10-17

The initial deployment repair left two operational items: registered repositories
were still backfilling vectors and historical audit verification was red. The
original assumption that the missing audit events could not be recovered was
superseded by inspection of the raw pre-recovery filesystem snapshot.

## Historical evidence

The logical SQLite file returned zeros for 73 table pages (SQLite page numbers
3778179–3778251). The corresponding ext4 mappings were marked unwritten. Reading
those physical blocks directly from the pre-audit-recovery-20261014 snapshot,
without mounting or modifying its filesystem, recovered the original pages.
All 876 missing events, sequences 45210997–45211872, were present. Each extracted
row's v2-full hash and every link verified, including adjacent original rows
45210985–45211896. No timestamp, payload or existing hash was synthesized.

A separate derivative of the original database was patched only at the 73 zero
pages, with assertions that the logical destination bytes were all zero and the
raw-page digests matched the extraction receipt. SQLite integrity_check passed;
all 45,907,066 sequence numbers are present. The damaged original's SHA256 remains
`a0425f6dd505f70de80c4f307581ce75b519823b87e8aabee04ff7d5c75d2ac6`.
A compressed backup was decompressed and verified byte for byte before relocation
from the appliance to backup storage; the original snapshot also remains.

The restored derivative's pre-attestation SHA256 is
`27250bee1196bc1cf48dc7757281eca22889c957f9fabd9576ea1d2721fb8621`.
The operator helper verifies the complete original chain and every checkpoint MAC
before appending one new checkpoint at the current time. That new row attests the
recovered historical head; it does not change existing evidence. The archive is
then switched to rollback-journal mode and made read-only. The active 20261014
recovery epoch remains separate and continues accumulating current events.

The verification API now checks audit/worm-restored.db instead of permanently
reporting red whenever a recovery epoch exists. Missing, mutable, empty,
unattested or damaged archives fail closed. Verified but uncheckpointed tails
remain amber. Successful verification is cached only for unchanged archive and
checkpoint-key file identities; changes and unresolved SQLite sidecars invalidate
it. Original loss manifests remain historical evidence, supplemented by restoration
receipts rather than rewritten.

## Repository indexing

The code index previously embedded every selected document serially and selected
all sixteen pending files from the first repository. It could also repeatedly
attempt cutover for the first sixteen incomplete repositories, preventing a later
complete one from becoming ready.

Each batch now selects files across repositories, permits at most two concurrent
embedding requests, and publishes each pair before starting the next. The shipped
embedder can coalesce those requests. Database publication remains serial and
retains fingerprint, exact embedding-input hash, dimension, serving identity and
watermark checks. Cutover candidates must be complete, and the existing
transactional validation repeats that check under the project publication lock.
Incomplete generations are never marked ready to clear a backlog.

## Tests and production qualification

The C audit suite passes locally and in the deployment-image builder. Regressions
cover absent, mutable and empty historical archives, checkpoint MACs, content
tampering, cached-file invalidation, checkpoint-key replacement and SQLite
sidecars. The additional key/sidecar regressions also pass in the local suite.

Disposable PostgreSQL tests pass for overlapping document requests with a maximum
of two in flight, fair batch selection, completed-project cutover behind more
than sixteen unfinished projects, stale source/model rejection, exact verified
vector reuse, and busy-embedder retrieval. Concurrency and stale-model cases also
pass under Go's race detector against that database.

The r4 hotfix was deployed with existing data mounts, environment and trusted
dashboard certificate preserved. The planned Proxmox maintenance reboot exercised
its cold startup: server readiness, an enrolled client's pinned/mTLS real Codex
completion and the model probe pass afterward. The original interrupted archive
verification was resumed as a systemd task independent of the SSH session.
Full chain and checkpoint-MAC verification then passed for the restored archive.
Its final SHA256 is
`4d8d282356c92051924851dbb3cc9d09513e1777c0729e95e393ec1e0ec27dd7`.
The new checkpoint is sequence 45907067; all 45,907,066 original rows were retained.
Final API verification and repository-backfill completion are recorded separately
in the standalone deployment validation report.
