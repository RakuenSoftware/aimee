# MR-02 closeout checklist

MR-02 is the active proposal after MR-01's completed closeout. The eight frozen
acceptance clauses remain unchanged. Component results below do not yet certify
final process validation.

| Gate | Implementation and validation | Remaining final check |
|---|---|---|
| A1: model upsert/edit cannot inherit user authority | Shared/private admission, linked correction drafts, retained versions and exact-draft reviews; packaged runtime and private-authority regressions | Final full race and T2/T3 process replay |
| A2: episode/policy protection across writers | Canonical same-key/update admission; automatic maintenance, lifecycle and folding admission; import preserves epistemic kind while ignoring imported authority | New packaged runtime and native import tests; final HTTP import/maintenance fixture |
| A3: one expected-version concurrent correction | Row-locked owner/ID/revision comparison and actor-scoped keyed receipts; concurrent correction and review tests | Final race and HTTP/MCP checks |
| A4: crash-safe replacement and retry | History, replacement, scope copies, audit, extraction jobs, invalidation and immutable receipt share one transaction; failure/disconnection regressions | Final race and restart checks |
| A5: durable publication after commit; duplicate/reordered delivery safe | Personal/shared transactional outboxes, ordered collection generations, atomic relation-consumer queue/checkpoint and idempotent replay | Final committed-connection consumer and deployment regeneration checks |
| A6: consumer restart/gaps and stale release | Durable consumer cursor, bounded canonical resnapshot on retention/owner gaps; canonical root/parent revision checks and explicit-completion release guards from MR-01 | Final consumer recovery and provider-boundary checks |
| A7: tombstones, payload conflicts and forged actor refusal | Scope-bound exact key/content tombstones; owner/principal/request-bound immutable receipts; independently verified host authority | Final race and process authority/retry checks |
| A8: prior/new versions, author/reviewer, effective time and exact draft | Shared changesets/WORM records and private retained versions/decision receipts; reviewed digest and separate reviewer; adjacent effective-time boundaries | Final audit, review and process checks |

Tombstones identify exact key/content and primary scope. They do not detect
paraphrases. Canonical mutation success does not claim consumer catch-up; neither
a queue checkpoint nor an old local generation grants release authority.
