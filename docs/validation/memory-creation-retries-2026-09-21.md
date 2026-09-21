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

Fresh `.253` HTTP/MCP and restart tests are added to the placement harness.
Their execution and immutable image evidence are pending; the previous 1,472
passing checks belong to the earlier deletion/scope-refusal revision, not this
creation change. This slice does not certify all MR-02 or MR-01–18 acceptance.
