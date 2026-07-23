# P7-witness-e1 witness record, sink contract, and drain core

- **State:** planned; production-uninvoked until E3 lands.
- **Depends on:** P7-reseal D3b, the kb audit chain
  (`src/db2/kb_audit_worm.c`), and the reseal outbox
  (`kb_vault_rewrap_worm`).
- **Enables:** E2's shared outbox and budget, and E3's drain worker, admission
  gate, and release-gate flip.
- **Umbrella:** `tiered-llm-p7-external-worm-witness.plan.md`.

## Why E1 carries no caller

The witness changes what "audited" means. Once a drain path exists that can mark
evidence delivered, a bug in encoding, digesting, or acknowledgement silently
converts undelivered evidence into apparently-witnessed evidence — the exact
failure the witness exists to prevent, and one no downstream test can detect
because the local database looks correct either way.

E1 therefore lands only the pieces that can be proven exactly and offline: a
canonical record encoding with fixed vectors, a digest that binds the complete
logical event, a sink interface whose default implementation refuses, and a
delivery state machine that is idempotent by construction. Nothing in E1 is
reachable from admission, from the reseal orchestrator, or from any HTTP route.
No schema changes. `kb_egress_release_allowed()` is untouched.

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

## 3. Sink contract

Add `src/modules/vault/vault_witness_sink.{c,h}` defining a small vtable, in the
style of the existing custody providers (`vault_custody_*.{c,h}`):

- `submit` — offer a batch of encoded records plus the batch's own digest;
- `acknowledge` — confirm, for a batch previously submitted, exactly which
  records the sink has durably accepted;
- `head` — read back the sink's current per-shard heads, so a drain can resolve
  its own uncertainty after a crash; and
- `capabilities` — declare whether the sink is genuinely append-only and whether
  it can return durable acknowledgements at all.

The default provider registered in every build **refuses**: it returns a typed
`unconfigured` error from every call, and it declares no capabilities. There is no
in-memory or on-disk "local sink" in production code. A test-only sink lives
behind the test harness, is never registered by production initialization, and
carries the same build exclusion the D3b deterministic test controls use.

A sink that cannot declare append-only storage and durable acknowledgement is
rejected at selection time with a typed error. Downgrading to a best-effort sink
is not a supported configuration, because a best-effort witness provides exactly
the guarantee the local database already provides.

Every sink call takes a caller-owned monotonic deadline. There is no unbounded
call, no implicit retry inside the vtable, and no blocking DNS: transport policy
follows the same `verify-full` numeric-host-or-`hostaddr` discipline the D3a
database seam established, for the same reason — synchronous resolution must not
escape the deadline.

## 4. Delivery state machine

Add the transport-agnostic delivery core. Its states, per batch:

`built -> submitted -> acknowledged` with `built -> abandoned` before submission
and `submitted -> uncertain` on any deadline, transport failure, or malformed
response.

Rules the implementation must make structural, not conventional:

- A batch is identified by the digest of its canonically-ordered records. The same
  logical set of records always produces the same batch id, so a resubmission
  after an uncertain outcome is recognizable to the sink as the same batch.
- `uncertain` is never resolved by guessing. It is resolved only by `head` or by a
  fresh `acknowledge` for the same batch id. An uncertain batch that cannot be
  resolved stays uncertain and its records stay undelivered.
- Acknowledgement is per record, not per batch. A sink that accepts a prefix
  acknowledges the prefix; the remainder returns to `built` for a later batch. A
  batch-level "success" that does not enumerate accepted records is treated as
  malformed.
- An acknowledgement for a record the drain did not submit, for a shard sequence
  that regresses, or for a record whose hash does not match what was submitted is
  a typed integrity failure that fails the whole batch. It never partially
  applies.
- Nothing in E1 releases a budget slot or writes to any table; the state machine
  returns what was accepted and leaves persistence to E2.

## 5. What E1 explicitly does not do

- No schema. No outbox table, no shard counter rows, no budget row.
- No admission gating, no boot-time fail-closed check, no change to
  `kb_vault_policy.c`.
- No drain worker, no scheduler, no background thread.
- No checkpoint cadence, no signing invocation from production paths.
- No claim, anywhere in code comments or docs, that external delivery exists.
  The umbrella's wording rule applies to every symbol name and log string added
  here.

## 6. Validation gates

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
- Every delivery transition, including replay of an identical batch, a prefix
  acknowledgement, a regressing sequence, an unsubmitted record, and a hash
  mismatch.
- The default sink refuses every call with the typed `unconfigured` error, and a
  sink declaring no append-only capability is rejected at selection.
- Deadline exhaustion at submit and at acknowledge leaves the batch `uncertain`,
  never `acknowledged`.

### ASAN / UBSAN

- Encode/decode fuzz over truncated and corrupted buffers.
- Every delivery success and failure path, asserting no leak and no callback
  after a fail-closed transition.
- Sensitive-buffer canaries around the record builder, proving no key material is
  ever copied into a witness record even when the source event is a vault
  key-use event.

### Lint / build integrity

- Full lint and build with the test-only sink excluded from the production build,
  asserted the same way D3b asserts its deterministic test controls.
- No new production symbol reachable from admission, HTTP routing, or the reseal
  orchestrator; asserted by a link-level or symbol-level check rather than by
  review.

## 7. Deferred to later slices

- The shared outbox, shard counters, and the atomic unwitnessed budget (E2).
- The drain worker, ownership lease, checkpoint cadence, continuous chain
  verification, boot fail-closed, and the release-gate flip (E3).
- The full restart and signal-level kill matrix (E4).
