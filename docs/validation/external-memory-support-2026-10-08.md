# Cognee and Hillock conversation support — 2026-10-08

Scope: finish Aimee-owned integration support under [external conversation v2](../../integrations/memory/CONTRACT.md), using disposable guests on Proxmox `.253`. Native is exempt from the external contract. The existing production Discord guest and backend routing were untouched.

## Implemented

Both selected external owners use independent durable adapter catalogs without opening native record storage. They support records, exact revisions/history, correction and idempotent retries, scoped recall and personal/shared composition, evidence briefing, authenticated generic export, current-source revalidation and send barriers, and erasure with fresh host-captured admission epochs. Repeated completed erasure requests preserve subsequent fresh records; stale writers and retained-control restores cannot revive erased content. Expired send leases remain barriers until explicit completion; expiration alone does not establish whether dispatch occurred.

Capabilities publish profile, canonical owner, candidate selection, limits and non-exhaustive retrieval. Typed failures preserve capacity, unsupported and unavailable outcomes instead of empty successful searches. Cognee supports Vault-backed API keys; lab service credentials no longer depend on expiring login JWTs. The lab rotation explicitly overwrites the old Vault credential in a one-shot helper.

Portable transfer preserves canonical IDs, revision owner/tokens, authorship, history, validity and optional metadata, and merges retained erasure controls. The normal build installs `aimee-memory-transfer`. The native maintenance script bridges native rows/history without requiring native to implement the external contract. Native-only graphs, vectors, learning and control-plane services are outside this record snapshot.

## Validation and limits

Fresh existing test guests: CT9213 (Cognee 1.6.2) and CT9214 (Hillock pinned `1edd166ead75b85a9ab95cd6ba4faf7011ad567c`), enrolled mTLS, persistent identities/homes and supervised Go owners. CT9212's previously validated native fixes remain separate.

Final acceptance results:

| Check | Cognee | Hillock |
|---|---:|---:|
| Shipping CRUD/history/recall/export/lifecycle/outage | 16/16 | 16/16 |
| Independence, 257-record selection, actual-author erasure, stale restore, fresh admission | 7/7 | 8/8 (includes rank diagnostic probes) |
| Source barriers, exact shared history, briefing, profile capabilities | 4/4 | 4/4 |
| Required real provider contract | Passed, Cognee 1.6.2 | Passed, pinned upstream; two restart rounds |
| Service resume: fresh admission and briefing | 5/5 | 5/5 |
| Guest reboot: mTLS, both owners, unchanged erasure controls | 4/4 | 4/4 |

Race tests pass for the generic contract, common memory owner/catalog, both adapters and egress. The new Cognee pool/index-retention regression also passes in its guest. C boundary/ownership, module descriptors/process contracts, proposal links/reconciliation and generated reference checks pass. Proposal reconciliation reports pre-existing warnings outside this work. Portable record transfer passes 6/6 checks. Guest reboot results are retained separately below in the evidence summaries.

Safe summaries are stored in [the evidence directory](external-memory-support-2026-10-08/). Raw Vaults, private process environment snapshots, tokens, certificates, canonical memory snapshots and private SQL diagnostics are excluded.

The 257-record test initially exposed Cognee cold indexing exceeding the shipping deadline when indexing 256 candidates. The adapter now caps synchronous candidate indexing at 16 and retains still-valid earlier derived datasets outside the current query pool. It searches only the current admitted candidate names and revalidates exact canonical heads. Hillock ranks at most 256 candidates per request. Both scan the full eligible catalog to select their lexical pool; neither promises exhaustive semantic recall. The catalog snapshot bound is 64 MiB.

Cognee uses a deterministic local inference/embedding fixture. The provider is real, but these checks establish integration and lifecycle behavior rather than real-model retrieval or answer quality. Hillock uses its actual subword encoder/HYDRA scorer. Diagnostic negation/paraphrase probes validate returned identities, not correctness of an eventual chatbot answer.

Native → Cognee catalog → Hillock catalog → native transfer uses two synthetic records and two retained history rows in a disposable cloned schema. It checks exact content/IDs/revisions/authorship, occupied-target refusal, retained controls and actual erased-ID restoration refusal. It does not claim migration of every native derived feature or large production datasets.

Remaining work is [representative scale and answer quality](../proposals/pending/external-memory-engine-scale-and-quality.md), including real-model Cognee comparisons and complete Discord trajectories. Hillock graph/extraction/learning defects remain upstream optional-profile blockers, bypassed by the stateless ranker. Core integration completion does not assert identical native feature behavior or 100% production suitability.
