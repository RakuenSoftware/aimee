# MR-02 closeout checklist

MR-02 is complete as of 2026-09-24. All eight frozen acceptance clauses remain
unchanged. The [final closeout](../../validation/memory-mr02-closeout-2026-09-24.md)
records the exact image, process counts, actual exits and verification boundaries.

| Gate | Implementation and validation | Completed final check |
|---|---|---|
| A1: model upsert/edit cannot inherit user authority | Shared/private admission, linked correction drafts, retained versions and exact-draft reviews; packaged runtime and private-authority regressions | Full race and T2/T3 process replay passed |
| A2: episode/policy protection across writers | Canonical same-key/update admission; automatic maintenance, lifecycle and folding admission; import preserves epistemic kind while ignoring imported authority | Packaged runtime, native import, HTTP authority and scoped-credential fixtures passed |
| A3: one expected-version concurrent correction | Row-locked owner/ID/revision comparison and actor-scoped keyed receipts; concurrent correction and review tests | Race and HTTP/MCP checks passed |
| A4: crash-safe replacement and retry | History, replacement, scope copies, audit, extraction jobs, invalidation and immutable receipt share one transaction; failure/disconnection regressions | Race and restart checks passed |
| A5: durable publication after commit; duplicate/reordered delivery safe | Personal/shared transactional outboxes, ordered collection generations, atomic relation-consumer queue/checkpoint and idempotent replay | Committed-connection consumer and deployment regeneration checks passed |
| A6: consumer restart/gaps and stale release | Durable consumer cursor, bounded canonical resnapshot on retention/owner gaps; canonical root/parent revision checks and explicit-completion release guards from MR-01 | Consumer recovery and provider-boundary checks passed |
| A7: tombstones, payload conflicts and forged actor refusal | Scope-bound exact key/content tombstones; owner/principal/request-bound immutable receipts; independently verified host authority | Race and process authority/retry checks passed |
| A8: prior/new versions, author/reviewer, effective time and exact draft | Shared changesets/WORM records and private retained versions/decision receipts; reviewed digest and separate reviewer; adjacent effective-time boundaries | Audit, review and process checks passed |

Tombstones identify exact key/content and primary scope. They do not detect
paraphrases. Canonical mutation success does not claim consumer catch-up; neither
a queue checkpoint nor an old local generation grants release authority.

There is no remaining MR-02 closeout work. MR-03 is next.
