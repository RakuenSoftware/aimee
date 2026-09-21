# Durable creation retries in the Go memory owner

Shared and private `store` accept an authenticated `idempotency_key` without an
`expected_version`. Each key binds the owner, principal, admitted store fields,
scope and effective authority. Existing insertion, same-key replacement and
review admission remain in force. An exact retry returns the original committed
identity only while its result is still visible, eligible and at that revision.
Changed requests conflict; erasure, supersession or expiry cannot reuse the key.

Creation receipts have schema version 2, outcome `stored`, the exact resulting
`version`, `commit_id` and `replayed`. A shared identical upsert has outcome
`unchanged`: its audit binds the existing revision without a memory mutation,
confirmation or evidence event. Admission can instead return the same linked
review proposal, including a previously rejected draft; deduplication preserves
its original author. Correction/deletion request hashes and receipts are unchanged.

Shared schema 28 and private migration 33 extend the existing receipt namespaces.
The canonical write, invalidation, extraction work and receipt commit atomically;
shared writes also seal their audit intent. Replays do not repeat those effects.
Private receipt identities do not claim shared WORM audit or downstream consumer
freshness. Unkeyed calls incur no retry lookup or key lock. No whole-request P95
improvement is claimed by this change.

Local validation:

- Full Go/PostgreSQL memory suite passes, including the restricted runtime role.
- Targeted race tests cover both authority modes, simultaneous same-key callers,
  committed response loss, disconnect before commit and replay after reconnect.
- Receipt insertion failures roll back canonical rows and audit effects; changed
  requests, forged result revisions, hidden/superseded/erased results, malformed
  keys and creation preconditions are refused. Unchanged shared stores retain
  revision, confidence and evidence count. Review admission and terminal proposal
  deduplication remain intact.
- Real shared schema 27 → 28 → 28 preserves previous correction and retirement
  receipts exactly; private schema 31 → 32 → 33 and reapplication preserve old
  correction/retirement identities and canonical history.
- Native build and memory routing checks, independent Aimee Go export with its
  embedded SQL, all 77 lint checks and all 17 S1 contract tests pass.

Fresh application/harness `15b756234fd7f2d34a4a00e4f69873c8a357b708` passes
**1,529/1,529 checks** on `.253`: **937 enrolled T2**, **592 standalone T3**.
Each private placement passes 214 memory checks; shared memory passes 273.
Creation cases cover authenticated HTTP and MCP, committed identity, changed-key
payload refusal, restart, model confidence limits, review admission and refusal
to recreate retired content. Provider and native worker checks continue to pass.

- [T2 verdicts and provider/native receipts](memory-creation-retries-2026-09-21/fresh-t2-15b756234f.json)
- [T3 verdicts and provider/native receipts](memory-creation-retries-2026-09-21/fresh-t3-15b756234f.json)
- [Image identities and cleanup](memory-creation-retries-2026-09-21/image-identities-15b756234f.json)
- [Schema upgrade evidence](memory-creation-retries-2026-09-21/schema-upgrade.json)

Application image: `sha256:c7f09a3cd3d5656e53f3bdacf75461ca33499d02106e20ff059a54f641d7c5b2`.
All nine container images and all three application instances' actual 32 KiB
provider caps were verified. After collection, the nine owned containers and nine
empty owned networks were removed; images, volumes and raw receipts remain.

The first creation run at `8441e0e2a2` passed all 592 T3 checks but stopped in T2
after 148 successful shared checks: its new harness read a nonexistent top-level
MCP `id`. The MCP contract carries the exact ID in the durable mutation receipt.
The corrected harness passed all 19 targeted creation checks against that retained
runtime before the complete fresh rerun above. The [diagnostic record](memory-creation-retries-2026-09-21/diagnostic-8441e0e2a2.json)
retains the incomplete run and its cleanup; those targeted checks are not added
to the complete run's totals.

These receipts cover the pushed creation implementation. Subsequent derived-parent
eligibility work is separate and is not validated by these images. This slice does
not certify consumer freshness or all MR-02 or MR-01–18 acceptance.
