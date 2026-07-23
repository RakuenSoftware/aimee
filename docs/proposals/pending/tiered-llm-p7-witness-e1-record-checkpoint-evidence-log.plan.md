# P7-witness-e1 witness record, checkpoint, export form, and evidence log

- **State:** planned; production-uninvoked until E2 lands.
- **Depends on:** P7-reseal D3b, the kb audit chain
  (`src/db2/kb_audit_worm.c`), and the reseal outbox
  (`kb_vault_rewrap_worm`).
- **Enables:** E2's emission, continuous verification, and release-gate flip.
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
  must be able to read during reconstruction has to be exported in the clear;
- the source entry's own hash and its predecessor hash, so the witness can verify
  linkage without holding the payload;
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

## 2. Signed-head checkpoint format

Add the checkpoint record to the same module. A checkpoint commits to the set of
per-shard heads at a defined instant.

**The commitment is a Merkle root, not a shard-head vector.** The number of
`(tenant, provider)` shards grows without bound over the system's lifetime, so a
checkpoint that serializes every shard head would grow without bound with it —
unbounded read, unbounded sort, unbounded signed output — and a monotonic
deadline would not make that bounded, it would only make checkpoint generation
start failing once the fleet got large enough — and the anchor that makes
exported evidence verifiable would quietly stop being produced.

The checkpoint therefore carries its own domain label, the checkpoint sequence,
the predecessor checkpoint digest, the shard count, and a single Merkle root over
the canonically ordered `(shard key, sequence, head hash)` leaves. The tree is
maintained incrementally as shards advance rather than rebuilt per checkpoint. A
per-shard inclusion proof is exportable separately and independently verifiable
against the signed root, so a consumer interested in one tenant retains a small
proof rather than the whole fleet's heads.

The checkpoint is signed under an existing vault-held key through the current
custody seam; E1 defines the format, the canonical serialization, the leaf
ordering, the root computation, the inclusion-proof format, the digest, and the
verification entry points. E1 does **not** wire checkpoint generation to a timer
or to any production scheduler — that is **E2**, which owns the "every N entries
or T seconds, whichever first" cadence together with emission.

Checkpoint verification is a pure function over bytes: given a checkpoint, its
predecessor digest, and a public verification key, decide valid or invalid with a
typed reason. It never consults the database, so a compromised database cannot
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
not a network client, not a state machine. Two exported surfaces, deliberately on
different transports:

- **Signed checkpoint export, over metrics.** The canonical checkpoint bytes,
  signature, Merkle root, and sequence, rendered so a consumer can verify offline
  with no access to aimee's database. This is small, bounded, and constant-shape,
  which is what a Prometheus sample can carry honestly.
- **Per-entry evidence export, over the log/OTLP path.** The witness records
  themselves, in shard order, each carrying its own hash, predecessor hash, and
  the non-secret identifying fields needed for reconstruction.

The transports are split on purpose. Prometheus exposition is a sampled snapshot,
not an event stream: evidence appearing between scrapes can be missed, repeated
samples may be deduplicated, and encoding record bytes into labels is unbounded
cardinality. Checkpoints tolerate sampling because each one commits to everything
before it. Per-entry records do not, so they ride the log path, which is
append-oriented and lossless in the way this needs.

The rendering is deterministic: the same evidence renders to identical bytes on
any instance, so copies retained by different consumers can be compared directly.
Rendering never consults mutable state outside the evidence itself.

Consumers are not enumerated, ranked, or tracked. Any authorized collector may
take the exports; more copies in more places is strictly better and requires no
bookkeeping on aimee. Every export call takes a caller-owned monotonic deadline.

## 4. Reservation identity and evidence grouping

Witness records are grouped so that reconstruction can recover *operations*, not
just a flat sequence of hashes. This is the surviving half of what an earlier
draft used for budget accounting; with no budget, grouping is what it is actually
for.

- Every witness record produced on behalf of an org key use carries the
  admission's dispatch key — `(tenant, authenticated_origin, request_id)` — as
  its **group id**. One admission may produce several records across the three
  source ledgers; they share one group id and are reconstructable as one
  operation.
- Operator-path evidence (rewrap and open events) never passes through admission
  and has no dispatch key. It carries its own group class keyed by
  `operation_id`, so it is grouped and reconstructable on the same terms rather
  than being silently ungrouped.
- The group id is part of the hashed record, so an attacker cannot regroup
  evidence without breaking linkage.

## 5. Evidence durability and what actually fails closed

Key use is already gated on a durable WORM append committing before the key is
attached — that is the existing P7 admission rule, not something this slice adds
or relaxes. That gate is what guarantees no org key is used unaudited, and it
operates entirely on aimee-kb's own committed state.

Export lag therefore does **not** gate egress. A collector that is down, slow, or
misconfigured reduces redundancy; it does not mean a key use went unaudited, and
halting all org egress because a metrics scraper died would trade a real
availability outage for no assurance gain. Export lag is an alerting signal with
a typed integrity alert, not an admission gate.

The honest claim, and the only one any slice may make: evidence is durably
committed on aimee-kb before key use, and is continuously copied outward so that
a later rewrite of aimee's own state is contradicted by copies the attacker
cannot reach.

## 6. Shared outbox and shard counters

Folded in from what was originally E2. These are schema and SQL only; no C caller
in this slice reaches them from a production path.

**Outbox.** `kb_vault_witness_outbox`, keyed by `(shard_key, shard_seq)`, holding
the encoded witness record, its hash and predecessor hash, the source
discriminator and source identity, the group id from §4, and the emitting seal
epoch and fencing token. Append-only by the same trigger discipline the existing
WORM tables use (`kb_worm_block` / `org_vault_rewrap_worm_block` in
`src/db2/schema.sql`), with UPDATE, DELETE, and TRUNCATE all rejected. Direct
table access is revoked from PUBLIC and from `aimee_kb_runtime`; the only paths
in are narrowly scoped SECURITY DEFINER functions with a pinned `search_path`,
matching the pattern D3a and D3b established.

"Outbox" is a name inherited from the drain-to-sink design and is now a misnomer:
nothing drains out of it and nothing empties it. It is the retained evidence
store, and E1 names it `kb_vault_witness_log` for that reason. Rows are never
deleted by any code path in this umbrella; retention is an operator policy
decision outside it.

**Shard counters.** `kb_vault_witness_shard`, one row per `(tenant, provider)`,
carrying the monotonic per-shard sequence and the current head hash. Rows are
created lazily by DML upsert — never by runtime DDL, and never by a PostgreSQL
`SEQUENCE` object — so the no-DDL invariant holds. The sequence is assigned by
`UPDATE … SET seq = seq + 1 … RETURNING` inside the caller's transaction, giving
each shard its own total order without a fleet-wide hotspot on a single head.

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
- No claim, anywhere in code comments or docs, that exported evidence has been
  durably retained by anyone. The umbrella's wording rule applies to every symbol
  name and log string added here.

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
- Checkpoint verification accepts a valid checkpoint and rejects a tampered
  leaf, a reordered leaf set, a wrong predecessor digest, a forged inclusion
  proof, and a wrong signature, each with a distinct typed reason.
- Merkle root stability: incremental maintenance and a full rebuild agree for the
  same leaf set, and the root is independent of shard insertion order.
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
  sequences and without DDL.
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

- Checkpoint cadence, metrics/log emission, continuous chain verification, the
  boot fail-closed check, and the release-gate flip (E2).
- The full restart and signal-level kill matrix (E3).
