# Memory backend qualification, 2026-10-11

Status: all four follow-up acceptance gaps qualified; PR CI and merge into testing pending.
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

The original synchronous graph-cognify diagnostic took 159.28 seconds. The final
CHUNKS route performs bounded parallel document classification, extraction and
embedding without graph LLM extraction. With pinned real Cognee 1.6.2 and deployed
Bekko A25M embeddings, the contract cold recall takes 11,031 ms. At 100, 1,000 and
10,000 records, cold recalls take 21,146 / 22,071 / 22,463 ms and queued warm p95
is 4,378 / 4,773 / 7,435 ms. All stay within 30 seconds with zero identity errors.
The embedding wire alias is `text-embedding-3-small`; the actual model is
`hotchpotch/bekko-embedding-v1-a25m`, pinned revision
`44f0b8af0f487acd0ccf1a7cb7ae7a29a6dfc09c`, 384 dimensions, identical empty query
and document prefixes. See `evidence/final-v2/cognee-real-*.json` and contract log.

## Validation and evidence

- Required real pinned Hillock contract passed, including restart and erasure.
- Required real Cognee contract with deterministic models passed after the changes.
- Memory/core/catalog/provider/egress Go packages pass race tests.
- 121 Discord adapter, projection, exact-token, worker and configuration tests pass.
- Six real adapter/guard checks pass for each of native, Cognee and Hillock.
  Assistant evidence can be omitted from bounded context, but exact owner reads
  prove its retained provenance and the omission receipt remains visible.
- Native PostgreSQL runtime replay passed initially in 278.77 seconds; the final
  isolated runtime-role replay passes in 152.86 seconds, including source-only
  creation retry intent, question, RLS, expiry and activation regressions.
- Module descriptors, source ownership, C memory boundary, process contracts,
  egress ownership and release approval topology pass. Proposal reconciliation
  remains report-only with pre-existing drift warnings.

See [evidence](evidence/) for safe synthetic JSON receipts, frozen answers,
provider summaries, contracts and test results. Private lab resume state, bearer
credentials, exported user stores and production conversation contents are excluded.

## Four follow-up acceptance gaps

1. Real-model Cognee cold retrieval now meets the 30-second budget through the
   bounded CHUNKS wrapper above; graph cognify is a separate operation.
2. The bridge counts the deployed model's rendered template and tokenizer before
   every generation and repair, including roles, BOS, delimiters and template
   arguments. It reserves the output budget, removes complete history turns and
   whole evidence records, and updates dependent projections and omissions.
   Missing tokenizer access and irreducible prompts refuse generation. The
   gateway's explicit preservation mode prevents hidden prompt augmentation;
   bridge responses require reported prompt usage to match the count. All four
   deployed gateway cases pass: multilingual text, role history, literal special
   delimiters and oversized history removed as complete turns (30/50/40/27
   counted tokens, respectively, exactly matching model usage).
3. Purpose, decisions, open questions, constraints, feedback and finite-lived
   preferences are durable pointers in the selected owner. Every pointer and
   control is recomputed against its canonical human episode before use. Bot
   repetition cannot confer human authority; revoked, deleted or expired sources
   are excluded. Literal episode admission bypasses factual extraction through
   `source_only`, which is included in creation retry identity. Eight fresh
   trajectory checks pass per backend: restart/topic return, pronoun after bot
   churn, person/place correction and feedback, roleplay then real correction,
   preference expiry, project privacy, source deletion and unavailable recall.
   Seven real Gemma4 E2B answers per backend pass with exact tokenizer receipts;
   the eighth outage case verifies no evidence is fabricated, without inference.
4. The live run passes for native, Cognee and Hillock in the existing channel
   `1557125396716191786`, guild `1532201714462101594`. Authenticated REST history
   supplies two actual human and three actual bot source messages per owner.
   Bot-authored fixture queries and replies are sent in private thread
   `1558834137350152292`; the exact thread ID is authoritative in the JSON receipt.
   The deployed `ModelClient.reply` uses each selected owner's prefetched context,
   the actual Unix gateway and mTLS Gemma4 E2B. All three count 990 prompt tokens,
   reserve 384 output tokens in the 2048-token limit, and verify provider usage
   equality. Each collection guard is acquired before Discord send, released
   after the confirmed receipt, and the actual bot response message ID is retained
   by that owner. REST verifies the delivered author. The thread is archived and
   locked. This is authenticated historical-source replay plus bot-authored live
   delivery, separate from synthetic human trajectories; it does not claim a new
   human or unlimited multi-bot conversation occurred in this run.

The existing bridge's earlier multi-bot, reconnect, attachment and workflow
validation remains in its 2026-10-10 reports. These checks concern the newly
introduced conversation-v2 path; no new channel designation is required.

Production candidate services retain the existing bot identity, workflow service,
personas, volumes and model weights. Native remains the selected production
owner; Cognee and Hillock are qualified through their isolated selected-owner
services. The three owners are alternatives, not simultaneous replicated stores.
The conversation-v2 profile is enabled after the exact gateway probe.

[PR #3020](https://github.com/RakuenSoftware/aimee/pull/3020) targets testing.
Required checks and merge remain pending at this report's preparation point.
Protected testing-to-main and release approvals remain their existing separate
workflow; this qualification does not claim a main release or exhaustive semantic
quality, corpus-scale indexing equivalence, or completion of broader research.

Final evidence lives in `evidence/final-v2/`. The final Cognee answer rerun uses
the existing knowledge Gemma4 E2B instance (same weights and template), capped by
the packer at 2048 tokens; native/Hillock use the existing 2048-token reply instance.
Live runs for all three owners use the actual reply gateway. It includes safe synthetic fixtures,
model answers and usage, test logs, and live metadata receipts. Private resume
state, credentials, provider databases and production chat contents are excluded.
