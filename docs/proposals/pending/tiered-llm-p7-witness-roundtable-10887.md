## Findings

### 1. Critical — Neither proposed confirmation candidate proves durable retention, so the watermark cannot safely release budget

**Evidence:** E1 §3, §4, §5, and §8.

The plan correctly rejects rendering or observing a scrape as acknowledgement, but neither remaining candidate establishes the property later sections rely on:

- A consumer read-back that echoes a checkpoint digest proves only that the consumer can return that digest at that moment. It does not prove that it durably retained the per-entry records needed for reconstruction, that the bytes survived a restart, or that they will remain available for the required retention period.
- A successful OTLP/forwarder delivery normally means that the next hop accepted a batch. It does not, absent a stronger consumer-specific protocol, mean that the terminal store durably committed it, retained the exact bytes, or will preserve them long enough for incident reconstruction.

The state machine makes this worse by treating a confirmation of the highest sequence and matching hash as evidence for the entire prefix. A consumer can possess entry `n` and its hash while missing entries `n-1`, `n-2`, etc. Neither `watermark = n` nor an echoed checkpoint digest proves contiguous retention of all records through `n`.

The plan therefore has no sound release event under the stated existing Prometheus/OTLP surfaces. A defensible confirmation would have to be an authenticated durable receipt from each downstream system, binding at least:

- consumer identity and configuration generation;
- shard and a contiguous retained range;
- the exact record bytes or a manifest/Merkle commitment covering them;
- the signed checkpoint anchoring that range;
- durable-commit semantics and the applicable retention obligation.

That is a new downstream contract. It need not be a third-party append-only sink, but it is more than ordinary Prometheus scraping or an OTLP transport acknowledgement. Until such a contract is selected and specified, E1's watermark and budget-release schema are not settled designs.

---

### 2. Critical — A confirmed watermark is permanent, but downstream retention is not; reconstruction can silently become impossible after budget release

**Evidence:** E1 §3 calls checkpoints something a consumer “retains cheaply and indefinitely”; §5 says release means “reconstructable from independent sources”; the umbrella says downstream services have “different retention.”

The design confirms retention at one instant and then permanently releases the unwitnessed slot. It defines no minimum retention horizon for per-entry records, no deletion notification, no lease/expiry on confirmation, and no mechanism to revoke or lower assurance when a downstream system ages data out.

This means the following is permitted by the plan:

1. Both consumers retain entries through sequence 100 and confirm watermark 100.
2. Aimee releases 100 budget slots.
3. Consumers age out the per-entry records while retaining only checkpoints.
4. The local chain is later found to have been altered.
5. The checkpoints demonstrate divergence but the preimages/history needed for reconstruction no longer exist.

The budget would have reported those uses as reconstructable even though that ceased to be true. A permanent watermark is only sound if the consumer's receipt commits to retention for an explicit horizon and incident detection/reconstruction is guaranteed within that horizon, or if the evidence is retained indefinitely. The documents specify neither.

---

### 3. Critical — The exported record is insufficient to reconstruct history; it preserves hashes, not the hashed history

**Evidence:** umbrella “Detection is local; reconstruction is the external job”; E1 §1 and §3.

The proposed witness record contains source identity, source hash, predecessor hash, shard sequence, epoch/fence, and timestamp. It does not contain the canonical source-row preimage or the logical event fields from which the source hash was computed.

A 32-byte source hash cannot be inverted to recover:

- the original audit event fields or payload;
- request ID and credential identity where not explicitly exported;
- the complete open-event row;
- the rewrap event's logical contents;
- transaction/operation grouping among related source-ledger rows.

Thus the export can recover a sequence of commitments and show which hash was previously observed, but it cannot reconstruct the events those commitments represented. That supports divergence adjudication or reconstruction of a **hash-chain skeleton**, not reconstruction of the “true history” as repeatedly claimed.

There is also an internal inconsistency: the umbrella says the witness carries `provider:cred` and request ID, but E1 §1's record field list does not include credential identity or request ID. A source-row hash committing to those values does not make the values reconstructable.

To meet the stated claim, the export must include a canonical, versioned representation of every non-secret source field needed to recompute each source hash, along with source ordering and operation/transaction relationships. If secrets prevent exporting complete source events, the assurance claim must be narrowed; hashes alone cannot fill that gap.

---

### 4. Critical — “Local proof of tampering” is not achievable against the document’s own privileged-database threat without a trusted rollback anchor

**Evidence:** umbrella “Why this is the remaining P7 scope” says a privileged PostgreSQL actor can rewrite the chain and outbox together; “Non-negotiable invariants” says the local chain proves tampering without external consultation.

A hash chain detects inconsistent edits. It does not detect a privileged actor who consistently rewrites or truncates:

- chain rows;
- shard head/counter rows;
- outbox rows;
- checkpoint sequence/head state;
- verifier progress/status.

The plan requires all load-bearing state to remain in PostgreSQL and identifies no TPM monotonic counter, independently retained last-seen checkpoint, transparency gossip, or other trusted local rollback anchor. A verifier restarting against a consistently rewritten database has no basis for knowing that a later valid state once existed.

Signed checkpoints do not fix this by themselves. Unless the verifier has a trusted record of the latest checkpoint outside the rewritable state, the attacker can roll the database back to an older valid signed checkpoint. If the host attacker can invoke the signing seam, it may also be able to obtain signatures over a rewritten successor history; the authorization and anti-rollback properties of that seam are unspecified.

This does not require changing the settled “stored on aimee-kb, copied outward” architecture, but it does invalidate the stronger claim that local verification alone proves all tampering covered by the stated threat. In the consistent-rewrite/rollback case, an external previously observed checkpoint is necessary even for detection.

---

### 5. High — Budget can advance beyond the last externally authenticated checkpoint

**Evidence:** E1 §2–§4.

Per-entry evidence is exported with hashes and predecessor hashes, but only checkpoints are signed. The watermark rules merely require that sequence `n` was exported and its hash matches. They do not require:

- a signed checkpoint whose shard head is at or beyond `n`;
- consumer retention of that checkpoint;
- verification that the retained records form the exact contiguous path to that checkpoint.

Consequently, budget can be released for unsigned tail entries. A transport or host attacker can substitute or fabricate an internally consistent tail, receive confirmation for it, and only later fail to produce a valid checkpoint. At the moment of release those records were not independently authenticated by the proposed signature scheme.

The safe release frontier cannot exceed the minimum, per consumer, of a retained and verified checkpoint head for which the complete intervening record range has also been durably retained.

---

### 6. High — The minimum-over-all-consumers policy creates an unbounded total egress outage with no assurance-preserving recovery path

**Evidence:** E1 §4–§5; umbrella “Budget exhaustion refuses new key use.”

Using the minimum across all configured consumers is internally consistent with requiring every named copy, but its availability consequence is stronger than the plan acknowledges:

- one dead, misconfigured, decommissioned, or retention-incompatible consumer stops budget release;
- after the finite backlog is consumed, all new key use halts;
- that halt is unbounded in time.

The backlog bounds the number of unwitnessed uses; it does not bound the outage. Fail-closed may be the correct security choice, but the design still needs an explicit recovery protocol. Simply removing the failed consumer from configuration would change the minimum and could instantly release slots, effectively turning an availability incident into an assurance downgrade. Adding a new consumer is equally undefined: it either starts at zero and deadlocks the fleet until historical backfill, or starts at the current watermark without having retained the history.

Consumer-set changes therefore need versioned membership, bootstrap/backfill rules, authenticated decommissioning, and a clearly auditable policy for whether unanimity or a fixed redundancy threshold is required. “Named and enumerated in configuration” is not enough.

---

### 7. High — The accounting unit is undefined and may either undercount or release slots too early

**Evidence:** E1 §1 and §5.

The witness covers three source ledgers, while the budget reserves `+1` “inside the admission transaction.” The plan does not define whether one admission creates:

- one witness record selected from one source ledger;
- multiple witness records, such as an audit row plus an open-event row;
- later related rewrap records;
- a grouped witness operation that is complete only after several records.

If one admission creates two records but reserves one slot, decrementing by crossed shard sequences can release more slots than were reserved or release the admission after only one constituent record is retained. Conversely, rewrap events appear not to pass through admission at all, so their undrained evidence may not consume any budget and is not bounded by the advertised unwitnessed ceiling.

The plan needs a precise reservation identity and completion predicate. A slot must correspond to a durable operation/group, and release must occur exactly once only when all evidence required for that group has met the confirmation condition. A raw sequence delta is not sufficient unless there is proven one-to-one correspondence between reservations and witness records.

---

### 8. High — Prometheus metrics and generic logs are not specified as a lossless evidence transport

**Evidence:** umbrella “Witness architecture”; E1 §3.

Prometheus exposition is a snapshot of metric samples, not an event stream:

- evidence appearing between scrapes can be missed;
- repeated samples may be deduplicated;
- arbitrary record bytes do not naturally fit numeric sample values;
- encoding records as labels creates high-cardinality, potentially unbounded series;
- collectors can truncate, relabel, reject, or normalize samples;
- a successful scrape says nothing about which downstream storage, if any, kept the samples.

Generic logs similarly have no inherent exact-byte, ordering, deduplication, or durable-retention guarantee.

The document never says whether `/v1/metrics` exposes all unconfirmed outbox records on every scrape, a moving bounded window, or only current heads. The first creates an ever-growing or ceiling-sized response; the latter choices permit missed entries and prevent reconstruction. “Deterministic rendering” only means identical input produces identical bytes; it does not make these transports complete or lossless.

A concrete paginated/ranged evidence protocol, or an explicitly lossless log/OTLP consumer contract with durable receipts, is required. The existing telemetry seam alone does not provide the stated semantics.

---

### 9. High — The global checkpoint design is unbounded and conflicts with the bounded-work invariant

**Evidence:** E1 §2; umbrella “Bounded work.”

Every checkpoint serializes “the set of per-shard heads” in canonical order. The number of `(tenant, provider)` shards is unbounded over system lifetime. Therefore checkpoint generation requires:

- reading all shard rows;
- sorting or producing all rows in canonical order;
- allocating/streaming an output proportional to every historical shard;
- signing and exporting an ever-growing record.

A monotonic deadline does not make this bounded; it merely makes checkpoint generation eventually fail once the fleet is large enough. If budget release requires checkpoints, this becomes a scale-triggered permanent egress outage.

The design needs a bounded authenticated structure—such as an incrementally maintained Merkle root with separately exportable proofs—or explicit paging with a signed top-level commitment. A full unbounded shard-head vector is incompatible with the stated invariant.

---

### 10. High — Checkpoint ownership is contradictory, allowing E2 to flip the gate before checkpoints exist

**Evidence:**

- Umbrella delivery split says E2 adds “the periodic global checkpoint.”
- E1 §2 says **E3** owns the checkpoint cadence.
- E1 §9 says checkpoint cadence is deferred to **E2**.
- Umbrella says E2 performs the release-gate flip, while E3 is the later kill matrix.

This is not editorial trivia because retained signed checkpoints are a prerequisite for independently authenticating the exported chains. If the E1 §2 assignment to E3 is followed, E2 can enable production egress and watermark release before any periodic checkpoint producer exists.

There is a second staging problem: the umbrella says P2b remains closed until every slice merges, but E2 itself changes `kb_egress_release_allowed()` while E3 has not yet merged. Unless there is a separate code-enforced latch, an E2 deployment can open production before the kill matrix gate has passed. Branch ordering and prose are not enforcement.

---

### 11. Medium — Folding E1 and the budget freezes semantics before the central release proof is designed

**Evidence:** umbrella “Delivery split”; E1 “Why E1 carries no caller” and §8.

“No caller” prevents immediate production impact, but it does not remove the schema and migration risk. E1 proposes to merge:

- watermark keys and granularity;
- budget counters;
- release accounting;
- consumer identity;
- outbox retention relationships;

while explicitly leaving the confirmation mechanism unresolved.

The eventual receipt contract may require range manifests, consumer generations, checkpoint IDs, expiry/retention terms, receipt signatures, or grouped-operation completion state. Those are not incidental fields; they determine the keys and transactional state model. Landing a scalar watermark and budget release model first can force an in-place semantic migration or preserve an unsound abstraction because E2 is expected to consume it.

The original separation addressed a real risk: proving what acknowledgement means before making budget semantics depend on it. The absence of a caller reduces deployment risk, but not design lock-in or migration risk.

---

### 12. Medium — The stated shard allowance does not provide the tenant fairness it claims

**Evidence:** umbrella non-negotiable invariants; E1 §5.

A per-`(tenant, provider)` allowance under a global cap can stop one shard from consuming more than its shard allowance, but it does not establish that:

- one tenant cannot consume the global budget through many providers/shards;
- several saturated shards cannot fill the global cap and starve an unrelated shard;
- inactive shards retain reserved capacity.

The SQL sketch also shows `shard_unwitnessed` as though it were a column in the single global budget row, despite shard counters being described separately. The locking and atomic update across global and per-shard rows are not specified.

The claimed isolation requires an explicit hierarchical reservation transaction and aggregate per-tenant limits or a fairness/reserved-capacity policy. Per-shard caps alone do not prove the stated property.

## Bottom line

The current plan does not yet define a truthful confirmed-watermark mechanism. Both proposed candidates are unsound when interpreted as ordinary Prometheus read-back or OTLP delivery confirmation. More fundamentally, the exported data and retention contract support detecting or adjudicating hash divergence, but not reconstructing the underlying true event history as claimed.

The release gate should not be tied to this design until there is a concrete durable-receipt protocol, explicit retention semantics, contiguous-range proof anchored by signed checkpoints, and an exact definition of the evidence needed for reconstruction.
