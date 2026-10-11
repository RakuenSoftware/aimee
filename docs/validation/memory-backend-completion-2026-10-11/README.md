# Memory backend qualification, 2026-10-11

Status: implementation and disposable-lab qualification; **not released**.
The baseline independent native/Cognee/Hillock integration is brought onto the
current testing base. This follow-up fixes defects exposed by corpus and
conversation tests and adds a backend-neutral Discord episode profile.

## Changes found by qualification

- Move the portable transfer command under the owning memory module. Descriptor,
  module ownership and build checks now pass.
- Score lexical candidates once before sorting, preserving score/ID tie order.
- Native recall falls back to scoped current-row PostgreSQL lexical terms when
  an entire conversational question has no literal phrase match. The existing
  activation, suppression and expiry gates still apply. Eight questions formerly
  returned zero records; all eight now retrieve their fixture evidence.
- External stores retain opaque source metadata and project its `use_cases` and
  `source_session` fields after restart, correction and exact historical reads.
- Grant hardened KB runtime access to source guard functions, without exposing
  barrier/lease tables. The native adapter's real guard probe previously refused.
- Cognee retains acknowledged indexing completion for exact dataset IDs. Reopened
  backend instances and recreated datasets must index again. Reduce the advertised
  synchronous lexical candidate pool to four after measuring the previous pool's
  cold-query timeout; final source payloads are still re-read canonically.
- Public external lists prioritize the requested project and newest admissions.
  Transfer cursors retain ascending order. The conversation profile prioritizes
  retrieved channel episodes and recent source spans before generic identity.
- The optional Discord `conversation-v2` profile captures before inference,
  distinguishes bot/human episodes, retains exact input text, rebuilds after
  restart, packs whole records with coverage receipts, checks source versions,
  guards delivery and visibly handles capture/recall outages. Existing configs
  retain their `native-assertions` default; the knowledge example opts into v2.

## Measured results

Synthetic corpus results include an old lexical needle and canonical identity
checks. External warm requests include serial queue wait, matching the shipping
handler. Each external corpus has four workers and 12 warm samples.

| Records | Hillock cold / p95 | Cognee cold / p95, four candidates |
| --- | --- | --- |
| 100 | 156 / 656 ms | 9,925 / 1,736 ms |
| 1,000 | 578 / 1,889 ms | 8,962 / 1,735 ms |
| 10,000 | 925 / 3,711 ms | 10,066 / 5,948 ms |

Both providers passed the 30-second request deadline with zero recall/identity
errors. These Cognee scale runs use real Cognee 1.6.2 with deterministic local
inference/embedding fixtures. They do not qualify real-model cold indexing.
The previous 16-candidate diagnostic at 10,000 records measured 57,137 ms cold
and 24,934 ms queued warm p95 (two workers, four samples, 120-second budget).

Catalog-only p95 at 100/1,000/10,000 records changed from
38.6/367.8/2,680.9 ms to 27.0/265.0/2,613.7 ms. The 10,000-record cold selection
changed from 251.7 to 187.9 ms. Locking and full snapshot reads still dominate
large-catalog concurrency; the score optimization does not establish a new index.

Eight multi-speaker retrieval fixtures and eight real E2B answer fixtures pass
for each of native, Hillock and Cognee: exact units, person/place distinction,
original attribution versus repetition, conflicting claims, negation, fiction,
topic return and preferences/paraphrase. The answer harness preserves full model
outputs, usage and omissions. A correct fiction answer that the real desk stays
on the floor was initially marked wrong for omitting the word “joke”; the final
criterion requires the factual answer instead. Native evidence includes that
explicit scoring correction. These are frozen synthetic cases, not live Discord
messages or an exhaustive semantic-quality benchmark.

The real Cognee contract with Gemma4 E2B Q4 inference and Bekko A25M embeddings
passed in 159.28 seconds with an explicit 600-second diagnostic budget. It made
10 completion and 20 embedding calls. The initial one-minute HTTP calls failed
before indexing completed, including schema retries. This establishes a functional
real-model route, **not** acceptance within the bridge's 30-second timeout.
The embedding wire alias is `text-embedding-3-small`; the actual model is
`hotchpotch/bekko-embedding-v1-a25m`, pinned revision
`44f0b8af0f487acd0ccf1a7cb7ae7a29a6dfc09c`, 384 dimensions, identical empty query
and document prefixes. Native embedding conversion therefore preserves its space.

## Validation and evidence

- Required real pinned Hillock contract passed, including restart and erasure.
- Required real Cognee contract with deterministic models passed after the changes.
- Memory/core/catalog/provider/egress Go packages pass race tests.
- 52 Discord adapter, worker and configuration tests pass.
- Six real adapter/guard checks pass for each of native, Cognee and Hillock.
  Assistant evidence can be omitted from bounded context, but exact owner reads
  prove its retained provenance and the omission receipt remains visible.
- Native PostgreSQL runtime replay passed in 278.77 seconds, including question,
  RLS, expiry and activation regression checks.
- Module descriptors, source ownership, C memory boundary, process contracts,
  egress ownership and release approval topology pass. Proposal reconciliation
  remains report-only with pre-existing drift warnings.

See [evidence](evidence/) for safe synthetic JSON receipts, frozen answers,
provider summaries, contracts and test results. Private lab resume state, bearer
credentials, exported user stores and production conversation contents are excluded.

## Remaining acceptance and release gates

1. Real-model Cognee cold indexing within the conversational request budget is
   unqualified; the extended diagnostic does not remove that gate.
2. The bridge's assembled prompt is capped at 6,144 UTF-8 content bytes rather than
   exactly counted with the deployed tokenizer. The frozen E2B fixtures fit its
   context, but arbitrary conversation inputs/history are not thus qualified.
3. A richer structured purpose/decision/feedback projection with scoped preference
   lifetimes and all requested pronoun, deletion/private-source, expiry and feedback
   trajectories remains pending in the conversational proposal.
4. Live Discord gateway, mention, reconnect and unbounded multi-bot acceptance
   requires an explicitly designated test guild/channel. No messages were sent.
5. GitHub CLI authentication is unavailable. Feature PR creation, required CI,
   merge into testing, testing-to-main protected approval and the protected release
   approval have not occurred. The existing workflows enforce those gates.

The disposable native/Cognee/Hillock lab guests received the candidate memory
binary and synthetic fixtures. Production Discord services/configuration and
backend selection were not changed. Keep both pending qualification proposals
pending; no production-scale or identical retrieval behavior claim is made.
