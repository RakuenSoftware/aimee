# Private correction proposal and review validation

The Go memory owner now persists private model correction drafts outside serving
memory. Draft identity binds the collection owner, target ID/revision and screened
payload digest. Repeated suggestions reuse pending and rejected drafts. Inspection
is bounded and checks the surviving private parent; deletion cascades to draft
payloads.

Authenticated exact-draft approval or rejection produces an immutable durable
decision. Approval retains the model's principal, transport and confidence ceiling,
records the user reviewer separately and keeps the private memory ID stable.
Canonical content, authorship, retained history, invalidation and the final
proposal decision share a transaction. A review retry verifies current result
eligibility/version before returning the original decision. Retired or erased
results cannot be released by replay.

`POST /v1/memory/correction_proposals` and
`POST /v1/memory/review_correction` accept explicit `store=user`. Their default
remains `kb`, preserving the existing shared command meaning. Host adapters only
route complete envelopes and verified context; the memory module and its bus
producer/consumer remain Go, and the C bus is unchanged. Private store/supersede
refusals now include a linked proposal reference. Private ordinary mutation
idempotency keys remain unsupported and are still acceptance work.

Migration 30 also prevents a model upsert from clearing an elapsed validity
interval. Review metadata is covered by revision/history/invalidation triggers;
ordinary read accounting remains outside those content triggers. Runtime roles
can insert drafts and update only decision columns under the durable guard, but
cannot rewrite payloads, terminal decisions or erase draft rows directly.

## Local evidence

The full uncached memory and Aimee-family race suites pass with both required
PostgreSQL fixtures enabled (51.653 seconds and 1.413 seconds). The new restricted
runtime-role fixture checks persisted proposals despite canonical refusal,
deduplication, inspection, unchanged serving data, model review refusal, digest
mismatch, rollback after a late decision-write failure, preserved model and human
history authorship, retry without another canonical effect, rejected-draft
suppression, actual concurrent reviewer blocking, revocation and erasure.

Native Server forwarding and dispatch tests pass, including verified versus
unverified user context and exact decimal identifiers. Memory ownership,
C-boundary, schema synchronization, Server API conformance, route coverage,
CLI transport routing and dispatch capability checks pass. The latest pushed
predecessor `62fab510ea` completed full CI successfully (run 35525951584).

Fresh deployment evidence and CI for this implementation are still pending.
The MR-01–MR-18 acceptance matrix remains open; this change does not claim
completion of MR-02's remaining mutation verbs, private keyed retries, consumer
checkpointing, retention/restore or full freshness requirements.
