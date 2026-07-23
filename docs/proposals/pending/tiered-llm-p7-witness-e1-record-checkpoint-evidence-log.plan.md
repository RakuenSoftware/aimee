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
  must be able to read during triage has to be emitted in the clear;
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
  tamper-evident once witnessed;
- the emitting seal epoch and fencing token;
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
a **sparse Merkle tree (SMT) keyed by the SHA-256 of the packed shard key**, with
a fixed depth, whose leaves are the packed `(shard key, sequence, head hash)`.

The tree shape is named on purpose. "Incrementally maintained, canonically
ordered" is not a design — a sorted Merkle tree changes topology as shards are
inserted, so an insert is not bounded and proofs are not stable. An SMT has fixed
topology determined by the key hash, so advancing one shard head updates exactly
one root-to-leaf path: **O(depth) node writes, independent of how many shards
exist.** Empty subtrees have precomputed constant hashes and are never
materialized.

**Durable schema.** The tree is database state, not process memory — process
memory would violate the shared-state invariant and evaporate on autoscale
teardown:

- `kb_vault_witness_smt_node(level, idx, hash)`, primary key `(level, idx)`,
  holding only materialized (non-empty) internal nodes. Updated by upsert along
  the changed path inside the same transaction as the shard advance.
- `kb_vault_witness_checkpoint(seq, root, predecessor_digest, shard_count,
  signer_key_id, sig_alg, signature, created_at)`, append-only under the same
  trigger discipline as the evidence log, retaining every checkpoint. Retention
  matters: a consumer holding an old checkpoint must still be able to obtain the
  ones between it and the current head.

**Signer identity.** The checkpoint carries an explicit `signer_key_id`,
signature algorithm, and version tag. Rotation retains historical verification
keys so previously emitted checkpoints stay verifiable; a checkpoint whose key id
is unknown to the verifier is a typed failure, not a soft pass.

Consumers obtain verification keys **out of band** — an operator-provisioned
trust anchor, never the same surface the checkpoints arrive on. A signature
checked against a key fetched from the emitting host proves nothing against a
host attacker, who substitutes both. Signatures give transport integrity and
rotation hygiene; comparison against retained copies is what defends against host
compromise, and E1 must not imply otherwise.

**Predecessor linkage is advisory, not a verification precondition.** Each
checkpoint names its predecessor digest so a consumer holding a contiguous run
can verify continuity. A consumer holding a *gapped* set must still be able to
verify each checkpoint it holds on its own — signature plus root — because
emission gaps are normal, not evidence of tampering. Verification therefore takes
a checkpoint and an optional expected predecessor: absent it, the verdict is
"individually valid, continuity unproven," which is a distinct typed result from
"continuity broken."

**Inclusion proofs.** A per-shard inclusion proof is the O(depth) sibling path
authenticating one shard's leaf against a signed root. E1 defines the proof
format, generation, and verification. Proofs are emitted alongside checkpoints on
the log path (§3) — a signed root a consumer cannot connect to any shard is not
independently verifiable, which is the whole point.

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
Rendering never consults mutable state outside the evidence itself.

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

**The witness append must be atomic with that source append.** The witness row,
its shard-head advance, and the SMT path update commit in the *same transaction*
as the source WORM event. If the witness were derived asynchronously, a crash
between the source commit and the witness append would leave an admitted key use
with no witness row — and the claim that evidence exists before use would be
false for exactly the events an attacker would most want missing. E1 provides the
SECURITY DEFINER function that does all of it in one transaction; E2 calls it
from the admission path. This is an E2 release prerequisite, not something E3's
kill matrix is expected to discover.

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

## 6. Evidence log, shard counters, and Merkle state

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
each shard its own total order without a fleet-wide hotspot on a single head.

**Merkle and checkpoint state.** `kb_vault_witness_smt_node` and
`kb_vault_witness_checkpoint` per §2, under the same revoke/definer/append-only
discipline. Node upserts and the checkpoint insert are part of the same
transaction as the evidence append and shard advance, so the tree can never
commit a root that disagrees with the log.

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

## 8. Validation gates

### Unit / default build

- Exact wire vectors for the record and the checkpoint, byte-for-byte, including
  a vector for each source discriminator and a vector with the maximum-length
  shard key.
- Decoder rejection for every malformed shape: short buffer, long buffer, wrong
  version, unknown discriminator, empty shard key, over-long shard key, wrong
  hash length, declared lengths that do not sum, and trailing bytes.
- Field-boundary forgery: two distinct logical events whose naive concatenation
  would collide must produce different digests.
- C-versus-SQL digest agreement for the same logical event.
- Canonical shard ordering is stable and independent of insertion order.
- Checkpoint verification accepts a valid checkpoint and rejects a tampered leaf,
  a forged inclusion proof, an unknown signer key id, and a wrong signature, each
  with a distinct typed reason.
- Gapped checkpoints verify individually and return "continuity unproven" — a
  distinct typed result from "continuity broken" — so normal emission gaps do not
  read as tampering.
- SMT correctness: incremental path updates and a full rebuild agree for the same
  leaf set; the root is independent of insertion order; empty-subtree constants
  are correct at every level; advancing one shard writes O(depth) nodes and reads
  no other shard's leaf.
- Inclusion proofs verify against the signed root for present shards and fail
  typed for absent ones; a proof from one checkpoint does not verify against
  another's root.
- Signer rotation: a checkpoint signed under a retired key still verifies when
  the historical key is in the anchor set, and fails typed when it is not.
- Deterministic rendering: the same evidence renders byte-identically twice, and
  across two independently constructed module instances.
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
  sequences and without DDL; SMT node upserts under concurrent advances on
  different shards do not deadlock and converge to the same root as a serial
  application.
- Atomicity: the evidence row, shard-head advance, and SMT path update commit
  together with the source event or not at all, proven by injected failure at
  each statement.
- Concurrent appends to one shard produce a gap-free sequence with correct
  predecessor linkage under SERIALIZABLE retry.
- Connection loss at BEGIN, at the append, and at COMMIT never leaves a shard
  head advanced without its evidence row.

### Lint / build integrity

- Full lint and build with test-only controls excluded from the production build,
  asserted the same way D3b asserts its deterministic test controls.
- No new production symbol reachable from admission, HTTP routing, or the reseal
  orchestrator; asserted by a link-level or symbol-level check rather than by
  review.

## 9. Deferred to later slices

- Checkpoint cadence, log/OTLP emission, numeric metrics, continuous chain
  verification, the offline verifier tool, the atomic admission-path caller, and
  the boot fail-closed check (E2).
- The full restart and signal-level kill matrix, and only then the release-gate
  flip (E3).
