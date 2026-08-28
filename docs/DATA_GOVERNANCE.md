# Data governance, retention, and deletion

The deployment owner is the data controller for material stored in Aimee and must record purpose,
lawful authority, geography, processors, and approved recipients before ingesting personal or
restricted data. Aimee is not a substitute for legal review.

| Class | Examples | Default maximum | Deletion | Backup treatment |
| --- | --- | ---: | --- | --- |
| transient request data | request bodies, temporary files | 24 hours | process/session cleanup | excluded |
| conversation and workflow content | prompts, responses, work artifacts | 30 days | subject/project deletion | expires with source |
| memory and ingested documents | memories, chunks, embeddings, derived facts | 90 days | source plus derived deletion | expires within one backup cycle |
| operational logs | service and access logs | 30 days | scheduled rotation | expires within one backup cycle |
| WORM security evidence | bounded action/verdict/identity records | 365 days | never row-deleted; legal hold may extend | immutable encrypted copy |
| credentials | tokens, keys, certificates | shortest provider lifetime | revoke and cryptographically erase | vault-only; no plaintext backup |

These are conservative policy defaults, not a claim that every current storage engine contains a
built-in scheduler. Until a built-in reaper/subject-erasure command is available for a data family,
the operator must enforce the maximum in the database/volume lifecycle and record evidence. A
deployment that cannot do so must not ingest regulated personal data.

A subject request is authorized against the verified subject, assigned a non-content request ID,
and applied to mutable source plus derived rows. The completion evidence contains only request ID,
pseudonymous subject digest, store/count summary, policy revision, actor, time, and outcome. It must
not copy deleted content. Existing WORM rows are not altered. Legal hold suspends mutable deletion
only for the documented scope and expiry and requires Security plus Legal approval.

DB1, DB2, audit files, and backups require encryption at the volume/database layer; Vault custody
does not encrypt those stores. Keys must be separate from the protected volume. Restore tests must
prove an RPO of 24 hours and RTO of 8 hours at least quarterly; evidence records start/end time,
snapshot identity, integrity checks, data loss, reviewer, and remediation.
