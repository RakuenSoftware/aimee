# P7-witness-e1 witness record, export form, shared outbox, and budget

- **State:** planned; production-uninvoked until E2 lands.
- **Depends on:** P7-reseal D3b, the kb audit chain
  (`src/db2/kb_audit_worm.c`), and the reseal outbox
  (`kb_vault_rewrap_worm`).
- **Enables:** E2's drain worker, admission gate, and release-gate flip.
- **Umbrella:** `tiered-llm-p7-external-worm-witness.plan.md`.

## Why E1 carries no caller

The witness changes what "audited" means. Once a drain path exists that can mark
evidence delivered, a bug in encoding, digesting, or acknowledgement silently
converts undelivered evidence into apparently-witnessed evidence — the exact
failure the witness exists to prevent, and one no downstream test can detect
because the local database looks correct either way.

E1 therefore lands only the pieces that can be proven exactly and offline: a
canonical record encoding with fixed vectors, a digest that binds the complete
logical event, a deterministic exported rendering, and a watermark state machine
that is idempotent by construction, plus the schema the later drain will use.
Nothing in E1 is reachable from admission, from the reseal orchestrator, or from
any HTTP route; the tables exist but no production path writes them.
`kb_egress_release_allowed()` is untouched.

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
- the source entry's own hash and its predecessor hash, so the witness can verify
  linkage without holding the payload;
- the emitting seal epoch and fencing token;
- a monotonic per-shard witness sequence; and
- the RFC 3339 timestamp of the source entry, not of the drain.

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

Add the checkpoint record to the same module. A checkpoint binds, at a defined
instant, the set of per-shard heads: for each shard, the shard key, its highest
witnessed sequence, and that entry's hash. Shards are serialized in a fixed
canonical order — byte order over the packed shard key — so two instances
computing the same checkpoint produce identical bytes.

The checkpoint carries its own domain label, the checkpoint sequence, the
predecessor checkpoint digest, the shard-head vector, and the count. It is signed
under an existing vault-held key through the current custody seam; E1 defines the
format, the canonical serialization, the digest, and the verification entry point.
E1 does **not** wire checkpoint generation to a timer or to any production
scheduler — that is E3, which owns the "every N entries or T seconds, whichever
first" cadence.

Checkpoint verification is a pure function over bytes: given a checkpoint, its
predecessor digest, and a public verification key, decide valid or invalid with a
typed reason. It never consults the database, so a compromised database cannot
influence the verdict.

## 3. Export contract

The witness is stored on aimee-kb and exported outward as metrics and logs. E1
defines the exported *form* and the export bookkeeping; E2 wires the actual
emission.

Add `src/modules/vault/vault_witness_export.{c,h}` defining the rendering and the
per-consumer watermark, not a network client. Two exported surfaces:

- **Signed checkpoint export** — the canonical checkpoint bytes plus signature,
  rendered so a consumer can verify offline with no access to aimee's database.
  This is the anchor a consumer retains cheaply and indefinitely.
- **Per-entry evidence export** — the witness records themselves, in shard order,
  each carrying its own hash and predecessor hash so a consumer can re-derive
  linkage between two checkpoints it holds.

The rendering is deterministic: the same evidence renders to identical bytes on
any instance, so two consumers scraping different instances can be compared
directly. Rendering never consults mutable state outside the evidence itself.

**Export is pull-based and therefore unacknowledged.** A scrape of `/v1/metrics`
tells aimee-kb nothing about retention. E1 must not pretend otherwise, and the
budget must never release on a render or a scrape hit. E1 defines a per-consumer
**confirmed watermark**: a durable record, per named consumer, of the highest
shard sequence that consumer has demonstrably retained. What constitutes a
demonstration is the central open question this plan puts to review — the two
candidates are a consumer read-back that echoes a retained checkpoint digest, and
a push path with delivery confirmation layered on the P9 forwarder. E1 lands the
watermark structure and its update rules; it does not invent a confirmation
mechanism that the transport cannot actually provide.

Consumers are named and enumerated in configuration, not discovered. An unnamed
scraper may read the metrics surface — it is already auth-gated — but it never
advances a watermark and never releases budget.

Every export call takes a caller-owned monotonic deadline. There is no unbounded
call and no implicit retry inside the module.

## 4. Watermark state machine

Per consumer, per shard, the watermark advances only monotonically, and only on
evidence of retention:

- Advancement is `UPDATE … SET watermark = :n WHERE watermark < :n RETURNING`,
  so concurrent instances cannot regress it and a replayed confirmation is a
  no-op rather than a second release.
- A confirmation naming a sequence aimee never exported, a hash that does not
  match what was exported at that sequence, or a shard the consumer is not
  configured for is a typed integrity failure. It advances nothing and it never
  partially applies.
- Budget release is derived from the **minimum** confirmed watermark across
  configured consumers, not the maximum. Evidence held by one consumer and not
  another is not yet reconstructable from independent sources, which is the whole
  point of the fan-out.
- Nothing in E1 invokes this from a production path; E2 owns the drain that calls
  it.

## 5. What E1 explicitly does not do

## 5. Shared outbox, shard counters, and the unwitnessed budget

Folded in from what was originally E2. These are schema and SQL only; no C caller
in this slice reaches them from a production path.

**Outbox.** `kb_vault_witness_outbox`, keyed by `(shard_key, shard_seq)`, holding
the encoded witness record, its hash and predecessor hash, the source
discriminator and source identity, and the emitting seal epoch and fencing token.
Append-only by the same trigger discipline the existing WORM tables use
(`kb_worm_block` / `org_vault_rewrap_worm_block` in `src/db2/schema.sql`), with
UPDATE, DELETE, and TRUNCATE all rejected. Direct table access is revoked from
PUBLIC and from `aimee_kb_runtime`; the only paths in are narrowly scoped
SECURITY DEFINER functions with a pinned `search_path`, matching the pattern D3a
and D3b established.

**Shard counters.** `kb_vault_witness_shard`, one row per `(tenant, provider)`,
carrying the monotonic per-shard sequence and the current head hash. Rows are
created lazily by DML upsert — never by runtime DDL, and never by a PostgreSQL
`SEQUENCE` object — so the no-DDL invariant holds. The sequence is assigned by
`UPDATE … SET seq = seq + 1 … RETURNING` inside the caller's transaction, giving
each shard its own total order without a fleet-wide hotspot on a single head.

**Budget.** `kb_vault_witness_budget`, holding the global unwitnessed count and
per-shard allowances. Reservation is atomic and inside the admission transaction:

```
UPDATE kb_vault_witness_budget SET unwitnessed = unwitnessed + 1
 WHERE unwitnessed < :global_ceiling AND shard_unwitnessed < :shard_allowance
 RETURNING …
```

Zero rows updated means the budget is exhausted and admission refuses. There is
no check-then-act, no per-instance threshold, and no degraded mode. The ceilings
are build-time constants on any key-holding kb, not operator config, so they
cannot be set to infinity to defeat fail-closed. Release decrements against the
minimum confirmed watermark from §4, and a replayed confirmation releases
nothing twice.

**Retention versus release.** A released budget slot does not delete the outbox
row. Release means "reconstructable from independent sources"; the row stays for
the reconstruction procedure and ages out under a separate retention policy that
E2 owns.

## 6. What E1 explicitly does not do

- No admission gating, no boot-time fail-closed check, no change to
  `kb_vault_policy.c`. The schema exists; nothing in the admission path calls it.
- No drain worker, no scheduler, no background thread, no metrics emission.
- No checkpoint cadence, no signing invocation from production paths.
- No claim, anywhere in code comments or docs, that witnessed delivery exists.
  The umbrella's wording rule applies to every symbol name and log string added
  here.

## 7. Validation gates

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
  shard head, a reordered shard vector, a wrong predecessor digest, and a wrong
  signature, each with a distinct typed reason.
- Deterministic rendering: the same evidence renders byte-identically twice, and
  across two independently constructed module instances.
- Every watermark transition, including replayed confirmation (no second
  release), regressing sequence, unexported sequence, hash mismatch, and an
  unconfigured consumer.
- Budget release derives from the minimum confirmed watermark, proven by a case
  where one consumer is far ahead of another.
- Deadline exhaustion during export leaves the watermark unadvanced.

### ASAN / UBSAN

- Encode/decode fuzz over truncated and corrupted buffers.
- Every export and watermark success and failure path, asserting no leak and no
  callback after a fail-closed transition.
- Sensitive-buffer canaries around the record builder, proving no key material is
  ever copied into a witness record even when the source event is a vault
  key-use event.

### Real PG17 on CT103

- In-place schema upgrade and clean install; schema-sync and the full P1 RLS gate.
- Append-only triggers reject UPDATE, DELETE, and TRUNCATE on the outbox.
- `aimee_kb_runtime` and every unrelated role are denied direct DML on all three
  new tables and denied every new function they are not explicitly granted.
- Concurrent reservation against a saturated global ceiling: exactly the ceiling
  is admitted, the remainder refuse, and the count never exceeds the ceiling.
- Per-shard allowance saturation on one shard does not refuse a different shard.
- Shard counter rows are created by upsert under concurrency without duplicate
  sequences and without DDL.
- Connection loss at BEGIN, at the reservation, and at COMMIT leaves no reserved
  slot without an outbox row and no outbox row without a reservation.

### Lint / build integrity

- Full lint and build with test-only controls excluded from the production build,
  asserted the same way D3b asserts its deterministic test controls.
- No new production symbol reachable from admission, HTTP routing, or the reseal
  orchestrator; asserted by a link-level or symbol-level check rather than by
  review.

## 8. Open question for review

The confirmed-watermark mechanism in §3 is the one part of this plan not settled
by existing code. Pull-based metrics scraping provides no retention signal, so
either a consumer read-back or a push-with-confirmation path on the P9 forwarder
must supply it. Choosing wrong makes the budget release on something that does
not mean what it claims, which would be worse than not gating at all.

## 9. Deferred to later slices

- The drain worker, ownership lease, checkpoint cadence, metrics/log emission,
  continuous chain verification, boot fail-closed, outbox retention, and the
  release-gate flip (E2).
- The full restart and signal-level kill matrix (E3).
