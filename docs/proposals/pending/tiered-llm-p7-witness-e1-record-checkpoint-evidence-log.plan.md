# P7-witness-e1 witness record, checkpoint, export form, and evidence log

- **State:** planned; production-uninvoked until E2 lands.
- **Depends on:** P7-reseal D3b, the kb audit chain
  (`src/db2/kb_audit_worm.c`), and the reseal outbox
  (`kb_vault_rewrap_worm`).
- **Enables:** E2's atomic admission caller, emission, and verification; E3's
  kill matrix and release-gate flip.
- **Umbrella:** `tiered-llm-p7-external-worm-witness.plan.md`.

## Why E1 carries no caller

The witness changes what "audited" means. A bug in encoding or digesting produces
evidence that looks well-formed locally and is worthless for comparison against
an exported copy — the exact failure the witness exists to prevent, and one no
downstream test catches, because the local database looks correct either way.

E1 therefore lands only the pieces that can be proven exactly and offline: a
canonical record encoding with fixed vectors, a digest that binds the complete
logical event, a signed Merkle checkpoint, a deterministic exported rendering,
and the evidence schema. Nothing in E1 is reachable from admission, from the
reseal orchestrator, or from any HTTP route; the tables exist but no production
path writes them. `kb_egress_release_allowed()` is untouched.

This mirrors the reseal split: D2b built the reconciler with no production
caller, and D3a built operator foundations that could not accept a secret. Same
discipline, same reason.

## 1. Canonical witness record

Add `src/modules/vault/vault_witness_record.{c,h}`, following the exact style of
`vault_reseal_receipt.{c,h}`: a compiler-layout-independent wire format with
explicit lengths, an encoder, a decoder that validates every field, a digest, and
an equality check.

The record is the *evidence*, not the audited event's payload. It carries:

- a fixed version tag and a domain-separation label
  (`aimee-vault-witness-v1`), so a witness record can never be confused with a
  reseal receipt, a rewrap outbox event, or an open event;
- the source discriminator — which local ledger the entry came from
  (`kb_audit_event`, `kb_vault_rewrap_worm`, `kb_vault_open_event`) — because the
  three have different identity shapes and a witness that cannot distinguish them
  cannot verify anything;
- the source entry's identity within that ledger (sequence for the audit chain,
  `(operation_id, event_kind)` for the rewrap outbox, `event_id` for the open
  event);
- the shard key `(tenant, provider)` in canonical bounded form;
- the non-secret identifying fields of the source event — request id, principal
  or team, and `provider:cred` — carried explicitly, not merely committed to by
  the source hash. A 32-byte hash cannot be inverted, so any field an operator
  must be able to read during triage has to be emitted in the clear.
  `provider:cred` here is the credential's **stable identifier** (which provider,
  which named credential slot) — never a credential handle, wrapped-key
  reference, or anything from which key material could be dereferenced. The
  canary gate asserts this at field level, not merely over the whole record;
- the source entry's own hash, plus its source-ledger predecessor hash **only
  where that ledger actually is a chain**. `kb_audit_event` is hash-chained and
  supplies one. `kb_vault_rewrap_worm` has deterministic event IDs and replay
  checks but no predecessor link, and D3b's `kb_vault_open_event` has a row hash
  but no predecessor. For those, the field is explicitly absent and the
  discriminator says so — never a zero placeholder, which would look like a
  verified link, and never the witness shard predecessor, which would conflate
  two different chains;
- the **witness** predecessor hash — the previous witness record in this shard.
  This is the linkage the witness chain owns and it exists for every source
  ledger, which is what makes a source ledger without its own chain still
  tamper-evident once witnessed. The first record in a shard has no predecessor
  and uses a **domain-separated genesis sentinel** —
  `SHA-256("aimee-vault-witness-genesis-v1" || packed shard key)` — not all
  zeros. An all-zero placeholder is forgeable as "first record" for a shard that
  already has history; a shard-bound sentinel is not. The decoder rejects the
  genesis sentinel on any record whose shard sequence is not the first, and
  rejects any other value on the first;
- the emitting seal epoch and fencing token. These are not new counters. The
  witness SECURITY DEFINER function reads them itself —
  `SELECT seal_epoch, fencing_token FROM kb_vault_control` — inside the appending
  transaction, so the values cannot be supplied by the caller and cannot drift
  from the lifecycle state in force at append time. Evidence appended by a stale
  instance that lost its fence therefore carries the superseded epoch and fence.
  The verifier reports that as a typed `stale_fence` result: a distinct triage
  signal, **not** a tamper detection — a fenced instance writing its own honest
  record is a liveness artifact, not an attack, and conflating the two would page
  on-call for a benign race;
- a monotonic per-shard witness sequence; and
- the RFC 3339 timestamp of the source entry, not of the export.

The record contains no credential, no key, no wrapped material, and no
fingerprint derived from key bytes. The decoder rejects any record whose declared
lengths do not sum to the buffer, whose shard key is empty or over-long, whose
hashes are not exactly 32 bytes, or whose source discriminator is unknown. There
is no "unknown field, ignore" path.

All variable-length fields are length-prefixed before hashing, using the same
pack-then-hash discipline as `org_vault_rewrap_pack_bytes` in `src/db2/schema.sql`
— a concatenation without explicit lengths is forgeable by field-boundary
shifting, and the existing SQL side already avoids that. The C and SQL packing
must agree; a test asserts identical digests for the same logical event computed
on both sides.

## 2. Signed checkpoint over a sparse Merkle tree

A checkpoint commits to every shard head at a defined instant. The commitment is
a **sparse Merkle tree (SMT) of fixed depth 64, keyed by the leading 8 bytes of
SHA-256 over the packed shard key**, whose leaves are the packed
`(shard key, sequence, head hash)`.

Depth is named, not left to the implementation. Depth 64 keeps collision
probability negligible for any realistic shard population (shards are
`tenant × provider`, so thousands, not billions: at 10^5 shards the birthday
bound is ~10^-9) while keeping proofs to 64 siblings. A shallower key would trade
proof size for a collision posture where two shards share a leaf, which is
disqualifying — a collision would let one tenant's head silently stand in for
another's. Empty subtrees have precomputed constant hashes per level and are
never materialized.

**The tree is built at checkpoint time, not on every shard advance.** An earlier
revision updated a durable root-to-leaf path inside the same transaction as the
source event. At depth 64 that is 64 node upserts on the admission hot path, in
the transaction that also carries the source WORM append and the chain-hash
computation — a large, unstated cost on exactly the path that must stay bounded.
So:

- **Per advance (hot path, atomic with the source event):** one evidence row and
  one shard-head update. Two rows. Nothing else.
- **Per checkpoint (E2 cadence, its own transaction, off the hot path):** read the
  shard-head table, build the tree, sign the root, persist the checkpoint and the
  leaf snapshot that proofs are generated from.

The checkpoint therefore *does* scan `kb_vault_witness_shard` once. That is
stated plainly rather than hidden behind "incrementally maintained": the scan is
O(shards), bounded by a documented maximum shard count, runs once per cadence
rather than once per key use. Exceeding the documented ceiling is a typed
failure, not a silent slowdown. Checkpoint construction carries its own monotonic
deadline — 1s typical, 5s hard — and expiry raises a typed
`checkpoint_deadline_exceeded` that aborts and alerts rather than signing late or
partially; the next cadence retries from the same committed state. The invariant this
design satisfies is "checkpoint cost is off the hot path and bounded by a stated
ceiling" — not "no scan exists."

Because different shards have unrelated key-hash prefixes, their leaves are
disjoint and concurrent advances never contend on shared tree state; there is no
tree state on the advance path at all.

**The rebuild must cross-check the shard heads against the log, or the shard-head
table stops being a paper trail.** Removing the durable node table has a cost
that has to be paid back explicitly: the checkpoint becomes a pure function of
`kb_vault_witness_shard` at scan time, and that table is a mutable current-value
row per shard, not an append-only history. An attacker on the host could insert
evidence rows (INSERT is not blocked by the append-only triggers — only UPDATE,
DELETE, and TRUNCATE are), advance the shard heads to match, and let the next
checkpoint be built from the modified heads. Every local artifact would agree
with every other one.

So before signing, the checkpoint **recomputes each shard's head by walking the
evidence log forward from the previous checkpoint's recorded head** and compares
it to `kb_vault_witness_shard.head_hash`. A mismatch is a typed
`head_log_mismatch` failure that aborts the checkpoint and raises an integrity
alert; it never signs. The walk is O(records appended in the cadence), which is
work the system already did once, and it hard-fails only on actual divergence.

This is what keeps the "locally inconsistent tampering is detected immediately"
claim true for the checkpoint path rather than only for the log. Without it, the
claim would have to be downgraded, because the only witness-side state feeding
the signature would be a table with no history.

**Durable schema.** `kb_vault_witness_checkpoint(seq, root, predecessor_digest,
shard_count, leaf_snapshot, signer_key_id, sig_alg, sig_version, signature,
created_at)`, append-only under the same trigger discipline as the evidence log,
retaining every checkpoint. Retention matters: a consumer holding an old
checkpoint must still be able to obtain the ones between it and the current head,
and proofs must remain generatable against historical roots.

**Signer identity.** The checkpoint carries an explicit `signer_key_id`,
signature algorithm, and version tag. Rotation retains historical verification
keys so previously emitted checkpoints stay verifiable; a checkpoint whose key id
is unknown to the verifier is a typed failure, not a soft pass.

**Trust anchor.** Consumers obtain verification keys **out of band** — an operator
provisioned anchor delivered through a channel independent of the emitting host,
not a file the kb host serves or a key fetched from the same surface the
checkpoints arrive on. A signature checked against a key the attacker also
controls proves nothing. On the kb side, chain verification loads its anchor set
at boot; **an anchor set that is missing, empty, malformed, or contains no key
matching a checkpoint's `signer_key_id` fails closed**, exactly as verification
being switched off does. Signatures give transport integrity and rotation
hygiene; comparison against retained copies is what defends against host
compromise, and E1 must not imply otherwise.

**Predecessor linkage is advisory, not a verification precondition.** Each
checkpoint names its predecessor digest so a consumer holding a contiguous run
can verify continuity. A consumer holding a *gapped* set must still be able to
verify each checkpoint it holds on its own — signature plus root — because
emission gaps are normal, not evidence of tampering. Verification takes a
checkpoint and an optional expected predecessor: absent it, the verdict is
`continuity_unproven`, a distinct typed result from `continuity_broken`.

**`continuity_unproven` is a work item, not a pass.** An attacker who knows
tooling treats it as clean can hide a fork behind a claimed emission gap: keep
local state consistent, suppress emission of the checkpoints spanning the fork,
and an operator comparing A, B, D, E finds each individually valid and stops. The
verifier must therefore make the gap actionable rather than merely labelled — for
every gap it reports, it surfaces the leaf set immediately preceding and
following, so the operator can compare heads directly across the gap. E1 defines
that output; E2's tool surfaces it. No slice may describe `continuity_unproven`
as a clean result.

**Two different attack shapes, and this affordance only covers one.** They must
not be conflated:

- **Missing checkpoint.** The attacker suppresses emission of checkpoints
  spanning the fork. The consumer's retained set has a hole. This is what the
  cross-gap leaf comparison above catches, because the leaf sets on either side
  of the hole disagree about the chain that ran between them.
- **Chain rewritten between two checkpoints that were both emitted.** The
  attacker cannot alter A or B — they are already off-host — but rewrites the
  local chain in the interval between them. A and B are individually valid,
  signed, and *mutually consistent with the rewrite*. The gap affordance never
  fires, because there is no gap. What catches this is the **retained record
  stream** between A and B: the emitted per-entry evidence the attacker also
  could not reach.

So the record stream is not redundant with the checkpoint stream — it is the only
thing that covers the second shape. A deployment that retains checkpoints but
drops records is protected against the first attack and blind to the second. This
is why the CT260 gate requires a consumer retaining *records*, not merely
checkpoints.

**Inclusion proofs.** A per-shard inclusion proof is the 64-sibling path
authenticating one shard's leaf against a signed root, generated from that
checkpoint's leaf snapshot. E1 defines the proof format, generation, and
verification. Proofs are emitted alongside checkpoints on the log path (§3) — a
signed root a consumer cannot connect to any shard is not independently
verifiable, which is the whole point.

E1 defines the format, canonical serialization, leaf encoding, root computation,
proof format, digest, and verification entry points. E1 does **not** wire
checkpoint generation to a timer or any production scheduler — that is E2.

Checkpoint and proof verification are pure functions over bytes plus a trust
anchor. They never consult the database, so a compromised database cannot
influence the verdict.

## 3. Export contract

The witness is stored on aimee-kb and exported outward as metrics and logs. E1
defines the exported *form*; E2 wires the actual emission.

**There is no delivery receipt, no per-consumer watermark, and no confirmation
protocol.** An earlier draft of this plan carried one, imported from the
hardened-vault §6 design where an off-host sink was authoritative and evidence
sat in a local outbox *waiting* to leave the host. In that design a backlog bound
had a real job: cap how many key uses could occur while their only durable copy
was still local and unreplicated. That is not this architecture. Here aimee-kb is
the system of record and retains the evidence; export is redundancy layered on
top. Nothing is ever in flight as the sole copy, so there is nothing to
acknowledge. Attempting to extract a cryptographic durable-retention receipt out
of a metrics pipeline would be inventing a protocol for a problem the
architecture has already solved, and every candidate mechanism would overstate
what a scrape or a transport ack actually proves.

Add `src/modules/vault/vault_witness_export.{c,h}` defining the rendering only —
not a network client, not a state machine.

**All evidence bytes ride the log/OTLP path.** Three emitted kinds, one
transport:

- **Witness records**, in shard order, each carrying its hashes and the
  non-secret identifying fields.
- **Signed checkpoints**, complete with root, sequence, predecessor digest, and
  signer key id.
- **Inclusion proofs**, so a consumer can authenticate a specific shard head
  against a signed root it holds.

The rendered stream carries an explicit export format version tag —
`aimee-vault-witness-export-v1`, distinct from the record's own version tag and
bound into the rendered bytes — so consumers comparing copies produced by
different aimee versions can tell "encoding changed" apart from "evidence
disagrees." Policy: a major bump on any byte-layout change, a minor bump on
purely additive fields. A verifier meeting an unknown or mismatched export
version returns typed `export_version_mismatch` rather than reporting a byte
comparison failure, which would otherwise read as tampering during a rollout.

**Metrics carry numbers only** — current checkpoint sequence, evidence count,
emission backlog depth, verification-failure counters. No bytes, no roots, no
signatures. An earlier draft put checkpoints on the metrics surface as "small and
constant-shape"; that fails against the Prometheus data model, where changing
roots and signatures as labels mint a time series per checkpoint and numeric
samples lose precision on 64-bit values. Exposition is also a sampled snapshot,
so checkpoints between scrapes would vanish silently.

**What the log path does and does not give us.** It is append-oriented and
ordered, which is what evidence needs, and it is the same seam P9's forwarder
work extends. It is *not* a guaranteed-delivery durable queue: OTLP and
application logging can drop under queue pressure, collector outage, process
crash, or retry exhaustion. E1 therefore claims only that evidence is *emitted*,
and the emission path reports what it can actually observe:

- local emission backlog depth, and
- failed or timed-out sends.

It never claims a collector retained anything, and it cannot report "a collector
is down" — with no consumer registry, aimee has no way to know whether an absent
scrape means an outage or simply no collector configured. Alerting on measurable
local state is honest; alerting on inferred downstream health is not.

The rendering is deterministic: the same evidence renders to identical bytes on
any instance, so copies retained by different consumers can be compared directly.
Rendering never consults mutable state outside the evidence itself — no
`time()`/`gettimeofday()` call, no hostname, no instance id, no locale-dependent
formatting, no map iteration order. Every timestamp in the output comes from the
record. This is a reviewable discipline, not only a property test, because a
property test can pass while a clock read sits on an untaken branch.

Consumers are not enumerated, ranked, or tracked. Any authorized collector may
take the emissions; more copies in more places is strictly better and requires no
bookkeeping on aimee. Every emission call takes a caller-owned monotonic
deadline.

## 4. Reservation identity and evidence grouping

Witness records are grouped so incident *triage* can see operations, not just a
flat sequence of hashes. This is the surviving half of what an earlier draft used
for budget accounting. Note the word: full reconstruction of what was done
belongs to the event bus, per the umbrella's invariant. Grouping makes the
witness side navigable; it does not rebuild history.

- Every witness record produced on behalf of an org key use carries the
  admission's dispatch key — `(tenant, authenticated_origin, request_id)` — as
  its **group id**. One admission may produce several records across the three
  source ledgers; they share one group id and are recognizable as one operation.
- Operator-path evidence (rewrap and open events) never passes through admission
  and has no dispatch key. It carries its own group class keyed by
  `operation_id`, so it is grouped on the same terms rather than being silently
  ungrouped.
- The group id is part of the hashed record, so an attacker cannot regroup
  evidence without breaking linkage.

## 5. Evidence durability and what actually fails closed

Key use is already gated on a durable WORM append committing before the key is
attached — that is the existing P7 admission rule, not something this slice adds
or relaxes. That gate is what guarantees no org key is used unaudited, and it
operates entirely on aimee-kb's own committed state.

**The witness append must be atomic with that source append — on every path, not
just admission.** The witness row and its shard-head advance commit in the *same
transaction* as the source event. If the witness were derived asynchronously, a
crash between the source commit and the witness append would leave a source event
with no witness row — exactly the row an attacker would want missing.

This applies to all three source ledgers, and the two operator paths are not
exceptions:

- **Admission path** — `kb_audit_event`, the append that gates key attachment.
- **Reseal orchestrator** — `kb_vault_rewrap_worm` events. These introduce new
  wrapped material, so they are among the most security-relevant evidence there
  is; witnessing them out-of-band would be worse than not witnessing them, since
  the gap would be invisible.
- **D3b open events** — `kb_vault_open_event`, the sealed-to-open transition.

E1 provides one SECURITY DEFINER function that appends the evidence row and
advances the shard head in the caller's transaction. **E2 wires it into all three
call sites**, which means E2 modifies the reseal orchestrator and the D3b open
function, not only the admission path. E1 §7's "no admission gating" exclusion is
about E1's own scope; it does not narrow E2's obligation to a single ledger.

**The three call sites have different transaction shapes, and this was verified
against the code rather than assumed:**

- **Reseal and D3b open are already in-transaction and are the easy cases.**
  Every `org_vault_rewrap_worm_append` call site is a `PERFORM` inside a SQL
  SECURITY DEFINER function (`src/db2/schema.sql`, the `intent`, `resealed`,
  `completed`, `abort`, and `recovery_required` transitions), and both
  `kb_vault_open_event` inserts are likewise inside definer functions. Adding the
  witness append to those functions is same-transaction by construction — no
  restructuring, no new transaction boundary.
- **The admission path needs the witness append moved inside an existing
  transaction.** `db2_kb_audit_append` (`src/db2/kb_audit_worm.c:44`) issues its
  own `BEGIN`, reads the current head, computes the row hash in C, inserts, and
  commits. It is a self-contained transaction that a caller cannot join. E2 adds
  the witness insert and shard advance *inside that function, before its commit*.
  This is a modification to an existing function, not a new transaction wrapped
  around it — wrapping would leave the same crash window this invariant exists to
  close.

One caution carried forward for E2: that function establishes its sequence and
predecessor with a read-then-insert (`SELECT … ORDER BY seq DESC LIMIT 1`,
then `INSERT`), which is not itself safe against concurrent appenders. The
witness shard advance must **not** copy that pattern — it uses the atomic
`UPDATE … SET seq = seq + 1 … RETURNING` on the shard row (§6). Whether the
pre-existing audit-chain pattern needs hardening is a separate question this
umbrella does not open, but E2 must not inherit it.

E1's validation gate proves the atomicity for all three ledgers, and it is an E2
release prerequisite — not something E3's kill matrix is expected to discover.

Emission lag does **not** gate egress. A collector that is down, slow, or
misconfigured reduces redundancy; it does not mean a key use went unaudited, and
halting all org egress because a log collector died would trade a real
availability outage for no assurance gain. Backlog depth and failed sends are
alerting signals, not admission gates.

The honest claim, and the only one any slice may make: evidence is durably
committed on aimee-kb before key use, and is continuously emitted outward so that
a later rewrite of aimee's own state is contradicted by whatever copies were
retained before the compromise. Evidence never emitted, or emitted but not
retained, has no external anchor.

## 6. Evidence log, shard counters, and checkpoint state

Folded in from what was originally E2. These are schema and SQL only; no C caller
in this slice reaches them from a production path.

**Evidence log.** `kb_vault_witness_log`, keyed by `(shard_key, shard_seq)`,
holding the encoded witness record, its own hash, its witness predecessor hash,
the source-ledger predecessor where one exists, the source discriminator and
source identity, the group id from §4, and the emitting seal epoch and fencing
token. Append-only by the same trigger discipline the existing
WORM tables use (`kb_worm_block` / `org_vault_rewrap_worm_block` in
`src/db2/schema.sql`), with UPDATE, DELETE, and TRUNCATE all rejected. Direct
table access is revoked from PUBLIC and from `aimee_kb_runtime`; the only paths
in are narrowly scoped SECURITY DEFINER functions with a pinned `search_path`,
matching the pattern D3a and D3b established.

It is deliberately not called an outbox. Nothing drains out of it and nothing
empties it; it is the retained evidence store. Rows are never deleted by any code
path in this umbrella; retention is an operator policy decision outside it.

**Shard counters.** `kb_vault_witness_shard`, one row per `(tenant, provider)`,
carrying the monotonic per-shard sequence and the current head hash. Rows are
created lazily by DML upsert — never by runtime DDL, and never by a PostgreSQL
`SEQUENCE` object — so the no-DDL invariant holds. The sequence is assigned by
`UPDATE … SET seq = seq + 1 … RETURNING` inside the caller's transaction, giving
each shard its own total order without a fleet-wide hotspot on a single head. The
row-level lock on the shard row is what serializes ordinals; concurrent appends
to *different* shards never contend.

Rows are created by the canonical upsert, not by runtime DDL:

```
INSERT INTO kb_vault_witness_shard(tenant,provider,seq,head_hash)
VALUES (:tenant,:provider,1,:head)
ON CONFLICT (tenant,provider)
DO UPDATE SET seq = kb_vault_witness_shard.seq + 1, head_hash = EXCLUDED.head_hash
RETURNING seq, head_hash;
```

**The concurrency contract is part of this slice, not left to the implementer.**
The witness append function retries a SERIALIZABLE conflict a **bounded** number
of times (5), because an unbounded retry loop on SSI conflicts is a
deadlock-amplifier rather than a fix. Exhaustion raises a typed
`witness_concurrency_exhausted`, and **the caller must treat that as a source
event abort**: if the witness row cannot be appended, the source event does not
commit. That is the atomicity invariant doing its job — failing closed rather
than committing a source event whose evidence is missing.

**Checkpoint state.** `kb_vault_witness_checkpoint` per §2, under the same
revoke/definer/append-only discipline. There is no durable Merkle-node table: the
tree is built from the shard-head rows at checkpoint time, so there is no
persistent tree state that could drift out of agreement with the log, and nothing
tree-shaped touches the hot path.

**No budget table.** There is no unwitnessed counter, no ceiling, and no
reservation. With evidence durably committed on aimee-kb before key use (§5),
there is no window of unreplicated-and-unaudited use for a budget to bound, and
with no confirmation protocol there would be no event that could ever release a
reserved slot — a budget here would only ever deadlock.

## 7. What E1 explicitly does not do

- No admission gating, no boot-time fail-closed check, no change to
  `kb_vault_policy.c`. The schema exists; nothing in the admission path calls it.
- No scheduler, no background thread, no emission.
- No checkpoint cadence, no signing invocation from production paths.
- No delivery receipts, watermarks, consumer registry, or budget.
- No claim, anywhere in code comments or docs, that emitted evidence has been
  durably retained by anyone, or that tampering "cannot go unnoticed" without the
  conditional-coverage qualifier. The umbrella's wording rules apply to every
  symbol name and log string added here.
- No offline verifier tool; that is E2, which also owns emission.

E2 must enforce the **inverse** of E1's reachability gate by the same link-level
or symbol-level check, not by review: the witness append symbol is reachable from
exactly the three source-ledger call sites and nowhere else. A softer assertion
at E2 would discard the precedent E1's gate exists to set.

## 8. Validation gates

### Unit / default build

- Exact wire vectors for the record and the checkpoint, byte-for-byte, including
  a vector for each source discriminator and a vector with the maximum-length
  shard key.
- Decoder rejection for every malformed shape: short buffer, long buffer, wrong
  version, unknown discriminator, empty shard key, over-long shard key, wrong
  hash length, declared lengths that do not sum, and trailing bytes.
- Field-boundary forgery, with a named vector the test must reject: event A with
  `source_id="42"` and `shard_key="acme:anthropic"` versus event B with
  `source_id="42acme"` and `shard_key=":anthropic"`. Their naive concatenation is
  byte-identical; length-prefixed packing must give them different digests.
- C-versus-SQL digest agreement for the same logical event.
- Canonical shard ordering is stable and independent of insertion order.
- Checkpoint verification accepts a valid checkpoint and rejects a tampered leaf,
  a forged inclusion proof, an unknown signer key id, and a wrong signature, each
  with a distinct typed reason.
- Gapped checkpoints verify individually and return `continuity_unproven` — a
  distinct typed result from `continuity_broken` — so normal emission gaps do not
  read as tampering; and the gap report carries the leaf sets immediately before
  and after each gap so an operator can compare heads across it.
- A fork hidden behind a **missing** checkpoint is caught when the cross-gap leaf
  sets are compared.
- A fork hidden by **rewriting the chain between two emitted checkpoints** is
  *not* caught by the verifier's gap handling — it is caught only by the retained
  record stream. The gate asserts this explicitly so nobody mistakes the first
  result for coverage of the second.
- Checkpoint rebuild cross-check: a shard head that disagrees with the head
  recomputed by walking the evidence log aborts with typed `head_log_mismatch`
  and never signs; injected head tampering is caught even when every local
  artifact is mutually consistent.
- A stale-fenced append is reported as typed `stale_fence` and is distinguishable
  from both a clean result and a tamper detection.
- Bounded SERIALIZABLE retry: exhaustion raises `witness_concurrency_exhausted`
  and the source event does not commit.
- `checkpoint_deadline_exceeded` fires on a stalled checkpoint rather than
  signing late, and the next cadence resumes from committed state.
- An unknown export format version returns `export_version_mismatch`, not a byte
  comparison failure.
- SMT correctness at depth 64: the root is independent of leaf insertion order;
  empty-subtree constants are correct at every level; two shard keys differing
  only beyond the 8-byte key prefix are detected as a collision and fail typed
  rather than sharing a leaf.
- Hot-path cost: one shard advance writes exactly two rows and touches no tree
  state.
- Inclusion proofs verify against the signed root for present shards and fail
  typed for absent ones; a proof from one checkpoint does not verify against
  another's root.
- Signer rotation: a checkpoint signed under a retired key still verifies when
  the historical key is in the anchor set, and fails typed when it is not.
- Deterministic rendering: the same evidence renders byte-identically twice, and
  across two independently constructed module instances. The rendering carries
  its own **export format version tag**, distinct from the record version tag, so
  a future encoding change cannot silently split the comparison universe into two
  mutually unverifiable halves.
- Group id binding: records from one admission share a group id, operator-path
  records carry their own class, and altering the group id changes the record
  digest.
- Divergence detection: a record whose exported copy differs from the stored
  copy in any field is detected by comparison, and a chain whose link is broken
  is detected locally with a typed reason.

### ASAN / UBSAN

- Encode/decode fuzz over truncated and corrupted buffers.
- Every rendering success and failure path, asserting no leak and no callback
  after a fail-closed transition.
- Sensitive-buffer canaries around the record builder, proving no key material is
  ever copied into a witness record even when the source event is a vault
  key-use event.

### Real PG17 on CT103

- In-place schema upgrade and clean install; schema-sync and the full P1 RLS gate.
- Append-only triggers reject UPDATE, DELETE, and TRUNCATE on the evidence log.
- `aimee_kb_runtime` and every unrelated role are denied direct DML on both new
  tables and denied every new function they are not explicitly granted.
- Shard counter rows are created by upsert under concurrency without duplicate
  sequences and without DDL.
- Checkpoint construction over a shard table at the documented ceiling completes
  within its deadline; exceeding the ceiling is a typed failure, not a silent
  slowdown.
- Atomicity, proven separately for **all three source ledgers** — admission
  (`kb_audit_event`), reseal (`kb_vault_rewrap_worm`), and D3b open
  (`kb_vault_open_event`): the evidence row and shard-head advance commit
  together with the source event or not at all, under injected failure at each
  statement.
- Concurrent appends to one shard produce a gap-free sequence with correct
  predecessor linkage under SERIALIZABLE retry.
- Connection loss at BEGIN, at the append, and at COMMIT never leaves a shard
  head advanced without its evidence row.

### Lint / build integrity

- Full lint and build with test-only controls excluded from the production build,
  asserted the same way D3b asserts its deterministic test controls.
- Operator-facing documentation states the conditional-coverage property: that
  external detection covers only evidence a collector retained before compromise,
  and that comparison depends on the deployment's collector ecosystem rather than
  travelling with the software. The umbrella's no-over-claim invariant is judged
  by the words that ship, so a gate reads them back.
- No new production symbol reachable from admission, HTTP routing, or the reseal
  orchestrator; asserted by a link-level or symbol-level check rather than by
  review.

## 9. Deferred to later slices

- Checkpoint cadence, log/OTLP emission, numeric metrics, continuous chain
  verification, the offline verifier tool, the atomic admission-path caller, and
  the boot fail-closed check (E2).
- The full restart and signal-level kill matrix, and only then the release-gate
  flip (E3).
