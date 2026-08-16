# P7-witness-e2 atomic append, emission, and continuous verification

- **State:** implemented and validated on branch `rewrite/go-server-wfe` (through
  `ecbfbc35`); merged to `testing` via PR #1930 (2026-07-24). Atomic witness append is wired into all
  three source ledgers (audit, reseal, open) — proven by the wiring + atomicity gate
  enforced from `run-p1-rls-gate.sh`. Emission on the log path, numeric-only health
  metrics, continuous chain verification with typed integrity alerts, the offline
  verifier tool, and boot fail-closed for a key-holding kb are all delivered. Does
  not touch `kb_egress_release_allowed()` (that flip is E3).
- **Depends on:** E1 (record, checkpoint, export form, evidence log), P7-reseal
  D3b, the kb audit chain, and the P9a telemetry seam.
- **Enables:** E3's kill matrix and, only after it, the release-gate flip.
- **Umbrella:** `tiered-llm-p7-external-worm-witness.plan.md`.

## What E2 is

E1 built the evidence machinery with no caller. E2 makes it real: it wires the
witness append into all three source ledgers, produces checkpoints on a cadence,
emits everything outward, verifies the chain continuously, and ships the offline
verifier an operator actually uses during an incident.

**E2 does not touch `kb_egress_release_allowed()`.** That function keeps
returning 0. The gate belongs to E3, after the kill matrix proves the crash and
restart behavior of everything E2 introduces. A test matrix that runs after
release is not a release gate.

## 1. Atomic witness append at all three call sites

E1 provides one SECURITY DEFINER function that appends the evidence row and
advances the shard head in the caller's transaction. E2 calls it from exactly
three places, and nowhere else.

### Shard model: one reserved shard per source ledger

The three source ledgers are **not** per-`(tenant, provider)` — they are
service-global or whole-vault chains. Verified against the code:
`db2_kb_audit_append` / `kb_audit_worm_append` carry only actor/action/subject;
`kb_vault_rewrap_worm` and `kb_vault_open_event` are whole-vault. So each ledger
maps to its **own reserved witness shard**, and the per-`(tenant, provider)`
sharding capacity is held for a future slice that witnesses per-key-use egress
with real identities.

| ledger | reserved `(tenant, provider)` | `source_kind` | `source_id` | `source_hash` | `has_source_pred` / `source_pred_hash` |
|---|---|---|---|---|---|
| audit | `("!kb", "!audit")` | 0 AUDIT | `seq::text` | the audit row's `row_hash` (a real content hash) | **true** / the audit row's `prev_hash` |
| reseal | `("!kb", "!reseal")` | 1 REWRAP | `operation_id \|\| '/' \|\| event_kind` | **`sha256(content pack of the rewrap row)`** — not `event_id` | false / zero |
| open | `("!kb", "!open")` | 2 OPEN | `event_id` | the open row's `row_hash` | false / zero |

Each reserved shard is its own tamper-evident sub-chain with its own checkpoint
leaf, so a rewrite of one ledger cannot be masked by another. The three chains are
already serialized upstream (audit by `pg_advisory_xact_lock`, reseal/open by the
vault maintenance barrier), so one-shard-per-ledger adds no hotspot.

**`source_hash` must bind row content, not identity.** For the audit and open
ledgers the existing `row_hash` is a true content hash, so the witness reuses it.
For the reseal ledger, `event_id` is `sha256(label ‖ operation_id ‖ event_kind)` —
a hash of *identity*, unchanged by a tamper of `state`, `seal_epoch`,
`receipt_digest`, or `detail`. Using it as `source_hash` would leave those fields
unwitnessed. The reseal witness therefore computes `source_hash` as a SHA-256 over
a canonical pack of the rewrap row's immutable content columns
(`event_id`, `state`, `seal_epoch`, `fencing_token`, the three digests, `actor`,
`detail`), reusing the same pack discipline as the digest helpers. The E2 PG gate
asserts that mutating any of those columns changes `source_hash`.

**`source_pred_hash` is the source ledger's own predecessor, distinct from
`witness_pred_hash`.** E1's record carries both: `witness_pred_hash` is the
previous witness record in the shard (always present); `source_pred_hash` is the
source chain's own predecessor and exists only for the audit ledger, which is
hash-chained. Reseal and open have no source-chain predecessor, so
`has_source_pred=false` and the field is zero. This separation is already built
in E1; E2 must not conflate the two.

**Collision safety with future real shards.** E2 writes only the three fixed
reserved keys, and the append function is the sole writer (direct DML on the
witness tables is revoked from every role). A real tenant/provider is never
written by this umbrella, so there is no collision to guard against in E2. The
obligation to keep real identities out of the `!`-reserved namespace transfers to
the future per-key-use slice, which introduces real `(tenant, provider)` values;
that slice must validate them against the reserved set. A blanket CHECK forbidding
`!`-prefixed shard keys is **not** usable here because the reserved shards
themselves use `!`.

**Reseal — `kb_vault_rewrap_worm`.** Add the call inside the existing definer
functions in `src/modules/db2/c/schema.sql` that already `PERFORM
org_vault_rewrap_worm_append`: the `intent`, `resealed`, `completed`, `abort`,
and `recovery_required` transitions. Each already performs its control-row
update, operation-row write, and WORM append in one plpgsql body, so the witness
append joins that transaction with no restructuring.

**D3b open — `kb_vault_open_event`.** Same shape: the completed-open and
idle-open functions already update `kb_vault_control`, insert the open event, and
`PERFORM` an audit append together.

**Admission — `kb_audit_event`, wired in SQL, not C (finding during implementation).**
The plan originally assumed the admission witness must modify the C
`db2_kb_audit_append`. Investigation showed otherwise: `kb_audit_event` has two
writers, and the security-critical one is SQL. Every `vault.key_use` (the append
that gates key attachment), reseal, open, rotation, catalog, registry, and status
event is written by the SQL `kb_audit_worm_append`, which already serializes
appenders under `pg_advisory_xact_lock`. **The witness append is wired there**, in
that transaction — so the key-attachment gate is fully witnessed, the witness
shard `FOR UPDATE` is never contended (the advisory lock already serialized), and
the high-blast-radius C admission-path change with its unsafe `MAX(seq)+INSERT`
sequence assignment is avoided entirely.

The C `db2_kb_audit_append` (`src/modules/db2/c/kb_audit_worm.c`) is the *other* writer, used
by only three call sites: two conspicuous integration-test overrides in
`kb_main.c` and artifact promotion in `artifacts.c` — none of them vault or key
security events. Witnessing that path (artifact-promotion audits) is a bounded
follow-up that would need the SERIALIZABLE-retry contract, because unlike the SQL
path it is not advisory-locked. It is **not** required for the umbrella's security
property, which is about vault/key events, all of which take the SQL path.

All three ledgers' wiring is validated on real PG17: `scripts/run-p7-witness-wiring.sh`
(isolated DB) proves audit/reseal/open each witness in-transaction with
content-binding source hashes and idempotent replay, and the full P1 RLS gate
passes with the audit witnessing active across every subsystem (no regression).

**The witness call must not inherit the audit chain's read-then-insert pattern.**
The C `db2_kb_audit_append` establishes its sequence and predecessor with
`SELECT … ORDER BY seq DESC LIMIT 1` followed by `INSERT`, with no advisory lock —
not safe against concurrent appenders (the SQL `kb_audit_worm_append` *does* take
`pg_advisory_xact_lock`, so the two writers to `kb_audit_event` differ in safety).
The witness shard advance uses E1's ensure-sentinel + `FOR UPDATE` instead.

A noted side effect, not a load-bearing fix: because the witness append takes
`FOR UPDATE` on the `(!kb,!audit)` shard row inside the same transaction, two
concurrent audit appends on the wired path serialize on that row and cannot both
commit the same audit `seq`. E2 must not *rely* on this to fix the audit chain —
the underlying C `MAX(seq)+INSERT` race is a pre-existing source-ledger defect,
out of scope here, and should be surfaced as a follow-up (a sequence-backed
`INSERT … RETURNING`, mirroring the witness store's own per-shard pattern) rather
than papered over.

On `witness_concurrency_exhausted` from E1's bounded retry, the caller aborts the
source event. Evidence that cannot be appended means the source event does not
commit — that is the invariant working, not an error to swallow.

**Reachability is enforced by tooling, not review.** E1's gate asserted no
production symbol reaches the witness code. E2 asserts the inverse by the same
link-level or symbol-level check against the **production** link map: the witness
append symbol is reachable from exactly these three call sites and nowhere else. A softer assertion here would
discard the precedent E1's gate exists to set.

## 2. Checkpoint cadence

A background producer signs a checkpoint every **N appended records or T
seconds, whichever comes first**, using E1's construction: scan the shard-head
table, build the depth-64 sparse Merkle tree, cross-check every head against the
head recomputed from the evidence log, and sign only if they agree.

The producer honors E1's rules exactly: fenced signer (take and revalidate the
`kb_vault_control` fence in the signing transaction, loser aborts before
signing), persist before emit, typed `head_log_mismatch` /
`checkpoint_shard_ceiling_exceeded` / `checkpoint_deadline_exceeded` failures
each raising an integrity alert.

### Architecture decisions (architect review 11384)

- **Signer: a vault-held witness Ed25519 key (not external KMS).** The umbrella's
  own invariant downgrades signatures to transport-integrity and rotation
  hygiene — comparison against retained copies is the load-bearing defense — so
  coupling the hot-path cadence (every N records / T seconds) to KMS availability
  buys a stronger posture than the threat model needs. This differs from the
  `vault_hwm` precedent (external KMS) because HWM is a one-shot externally
  published boundary marker, not a hot internal control loop. The signing is
  reached through a clean seam `vault_witness_sign(digest32, sig64)` whose current
  implementation is the vault-held key and behind which a future KMS
  implementation drops in with **no schema change** — the documented B→A path.
- **Key provisioning: reuse the vault KEK + custody seam.** The witness private
  key is a normal vault-keyed secret, sealed by the KEK under the selected custody
  anchor, generated during the existing vault provision step, unsealed in-process
  only to sign. Its public key is the out-of-band anchor consumers pin
  (`AIMEE_VAULT_WITNESS_PUBKEY`-style), recorded in E1's anchor set with the
  revocation the rotation story needs. Hardening for the in-process unsealed key:
  `mlock` the buffer and `explicit_bzero` it immediately after the sign call, no
  logging of the unsealed bytes. The witness key is **not** a release-gating
  artifact — holding it does not change the `kb_egress_release_allowed()` story.
- **C/SQL split: one C-driven `REPEATABLE READ` transaction.** Open txn → SQL
  revalidates the `kb_vault_control` fence (`FOR UPDATE` on the fence row only,
  never on shard heads) → SQL scans + cross-checks and returns the verified leaf
  set (`org_vault_witness_checkpoint_leaves`, done) → C builds the depth-64 root
  and signs → SQL persists the signed checkpoint → commit. Holding the txn across
  the in-process Ed25519 sign does **not** violate E1's no-txn-across-TPM rule:
  that rule targets blocking physical I/O; Ed25519 is microseconds, no I/O. This
  single snapshot is what makes the signed root match the persisted leaf set.
- **Cross-check walk: from the previous checkpoint's leaf snapshot** in
  production (O(records since last checkpoint)), read inside the same
  `REPEATABLE READ` snapshot. `org_vault_witness_verify_shard` currently walks from
  genesis — provably correct and the right primitive for the first-ever checkpoint
  and an operator `--from-genesis` rebuild, but O(all records/shard). The
  incremental walk is a **required follow-up before high-volume production**; it
  is not yet a load concern because the cadence scheduler is not wired.

### Witness signing-key lifecycle (verified property)

The witness Ed25519 key is `HKDF(server_kek)`. Checked against the code: nothing
rotates the server master key — `vault_server_kek` derives from a persisted
`<config_dir>/.vault/.server-master.key` generated once, and the whole-vault reseal
(`org_vault_rewrap` / the reseal orchestrator) does **not** touch it. Two
consequences, both worth stating rather than discovering later:

- **The witness key is stable across vault reseals.** A reseal rotates the
  custody-anchored KEK for DEKs; it does not change the witness signer, so
  previously signed checkpoints stay verifiable against the same anchor. There is
  no rotation break today.
- **There is also no witness-key rotation capability today.** When server-master-key
  rotation is introduced, the anchor set must retain the historical public keys
  (E1's anchor set already supports multiple keys plus revocation) or every
  checkpoint signed before the rotation becomes unverifiable. That retention rule is
  a prerequisite of any future rotation work, not an optional extra.

### Checkpoint SQL surface (built, PG17-validated)

- `org_vault_witness_verify_shard(tenant, provider)` — the head-vs-log cross-check
  (§ above), returns the verified head or raises `P7W01`.
- `org_vault_witness_checkpoint_leaves()` — one-snapshot scan of all shards,
  cross-checks each, enforces the 2^20 ceiling (`P7W02`), returns verified leaves.
- `org_vault_witness_checkpoint_persist(...)` — fenced monotonic-seq insert of an
  already-signed checkpoint (persist-before-emit). The C producer computes the
  root, predecessor digest, and signature, then calls this to durably commit.

A stalled checkpoint producer is a degradation, not a safe idle state: appends
and emission continue while new signed roots stop, so the unanchored window
grows. E2 surfaces the age of the latest signed checkpoint as a first-class
health signal for exactly this reason.

## 3. Emission

**Status: built and PG17-validated.** `src/modules/db2/c/db2_witness_emit.c` reads committed
state only, driven from the checkpoint cadence in `src/kb/kb_witness_cadence.c`.

**Log/OTLP path — all evidence bytes.** Witness records, signed checkpoints, and
their leaf snapshots, emitted as base64 of the exact export frame so a retained
line decodes straight into `aimee-witness-verify`. Emission reads committed state
only.

**Metrics path — numbers only.** Latest checkpoint sequence, latest checkpoint
age, evidence count, emission backlog depth, failed-send count, and
verification-failure counters, on the existing P9a surface
(`src/kb/http/kb_http_telemetry.c`). No bytes, no roots, no signatures.

Alerts fire on locally measurable state — backlog depth, failed sends, checkpoint
age — and never on inferred downstream health. With no consumer registry, aimee
cannot distinguish "collector is down" from "no collector configured," and must
not pretend to.

Emission never blocks admission. A full or failing emission path raises an alert
and drops nothing from the durable log, which is the system of record.

**Digest parity is enforced at emission, not assumed.** Each record is rebuilt
from its stored columns and its canonical digest compared against the stored
`record_hash` before framing. A mismatch halts the run with
`DB2_WITNESS_EMIT_PARITY_MISMATCH` and raises an integrity alert rather than
publishing: evidence that does not match the store is worse than no evidence,
because it looks well-formed yet can never match a retained copy. This discharges
the "emission re-encode parity test" item carried forward from the E1 review.

**The emission cursor is a position, not evidence.** It lives in
`kb_vault_witness_emit_cursor`, is deliberately not WORM, and carries no integrity
role. Advance is monotonic — a lower value is ignored, never applied — so a late or
duplicated advance cannot rewind the stream into a re-emission storm. Losing the
cursor entirely causes re-emission, which the offline verifier collapses as
byte-identical duplicates.

**Leaf snapshots are emitted and verified, not merely shipped.** A new
`VAULT_WITNESS_EXPORT_SNAPSHOT` frame carries `u64 checkpoint_seq` followed by the
exact stored snapshot bytes. The offline verifier hashes the payload and requires
equality with the `leaf_snapshot_digest` inside that checkpoint's *signed* body, so
a substituted or truncated snapshot cannot pass as the leaf set the signature
committed to. A snapshot whose checkpoint is absent from the stream is reported
`unmatched` — unverifiable, not tampered — so an operator knows to go fetch the
checkpoint rather than treating it as an attack. Consumers predating this frame
kind report it as an unknown frame, which was already tolerated and never counted
as tampering.

**Inclusion proofs are deliberately not emitted by default.** A proof lets a
consumer verify one shard's inclusion without the whole leaf set; with the leaf
snapshot emitted and digest-bound to the signature, any consumer can rebuild the
tree and check every shard directly, which strictly subsumes it. Emitting both
would duplicate bytes for no added assurance. The proof frame kind, wire format and
verifier path remain in place for a consumer that wants per-shard proofs without
the snapshot.

Validated end-to-end on real PG17: records plus a signed checkpoint plus its leaf
snapshot are emitted, and the captured bytes verify offline from the trust anchor
alone with no database access; a single flipped byte is detected; a second run with
nothing new appended is a no-op and a subsequent append emits exactly one record.
The offline core is clean under ASAN+UBSAN with leak detection.

### Findings from the emission review (job 11477 roundtable)

Two were real defects, found and fixed:

- **False tamper alarm on re-emission.** The offline verifier deduped records but
  not checkpoints, while a comment claimed otherwise. A byte-identical re-emitted
  checkpoint — which this emitter itself produces after a snapshot sink failure, and
  which a reset cursor produces for the entire run — was reported
  `CONTINUITY_BROKEN` with `any_tamper=1`, because the duplicate's
  `has_predecessor` is false in mid-run position. A healthy system retrying would
  have raised a hard tampering alarm. Checkpoints are now collapsed exactly as
  records are, and same-seq-different-checkpoint is reported as a fork. Reproduced
  before fixing.
- **The cursor could suppress emission.** Monotonicity alone did not prevent an
  advance far past the head, which would make the emitter skip all existing and
  future evidence while every gauge read healthy. Advances past the real stream head
  are now rejected (`P7W06`).

One assurance gap, closed: nothing checked that a snapshot's **leaves rebuild the
checkpoint's root**. Digest equality only proves the bytes are the ones signed; a
checkpoint whose root and snapshot digest did not correspond would have passed, and
the cross-gap comparison would have rested on a snapshot that describes a different
tree. The verifier now rebuilds the SMT root from the snapshot and requires equality
with the signed root, and the PG gate confirms the producer's stored format and the
verifier's parser agree.

Hardening: the offline core is clean under ASAN+UBSAN with leak detection; 20k
random streams and 60k bit-flip mutations of a valid stream produce no crash and
zero cases where a mutated stream verified as clean.

## 4. Continuous verification

**Status: built.** Two complementary halves, because a store can pass one and fail
the other:

- *Chain verification is continuous by construction.* The checkpoint producer's leaf
  scan calls `org_vault_witness_verify_shard` on every shard every tick, so a shard
  head that diverges from its evidence log raises `head_log_mismatch` and refuses to
  sign — verified on PG17, including that the producer refuses rather than
  certifying a divergent shard.
- *Signed-root verification* (`db2_witness_checkpoint_verify_run`, every 300s over a
  bounded window) reconstructs retained checkpoints from stored columns, verifies
  each signature against the current witness key, and checks predecessor continuity.
  "Could not run" is logged as unverified rather than passing silently, and
  `continuity_unproven` is surfaced as an operator work item — neither clean nor a
  tampering claim.

Boot fail-closed is built and extends beyond the original wording: a key-holding kb
now also refuses to start when any retained checkpoint names a `signer_key_id` it
cannot derive, and when the coverage check itself could not run. There is no
historical-anchor file because nothing rotates the server KEK yet; when rotation
lands it brings the anchor set and this check widens. **Validation-pending:** the
refusal path needs an environment where live keys are allowed; CT103 has none, so
the test reports that explicitly rather than claiming a pass it never exercised.

A background verifier walks the evidence log and checks: witness predecessor
linkage per shard, the genesis sentinel on first-in-shard records only, source
predecessor linkage where the source ledger is a chain, epoch/fence against the
control row's history (typed `stale_fence`), and every retained checkpoint's
signature against the anchor set including its revocation list.

Failures raise typed integrity alerts. Verification runs on a cadence and at
boot.

**Boot fails closed on a key-holding kb** when verification is disabled, when the
trust anchor set is missing, empty, or malformed, or when a retained checkpoint
names a `signer_key_id` absent from the anchor set. This matches the existing
non-disableable-WORM posture: a key-holding kb never starts unable to verify its
own evidence.

## 5. The offline verifier tool

A standalone tool that takes emitted bytes plus an out-of-band trust anchor and
verifies them with **no access to aimee's database** — because during an incident
the database is the thing under suspicion.

It verifies a checkpoint's signature and root, an inclusion proof against a
signed root (honoring the key-hash bit ordering E1 pins), and a record range's
linkage between two checkpoints. It reports `continuity_unproven` versus
`continuity_broken` distinctly, and for every gap it surfaces the leaf sets
immediately before and after so the operator can compare heads across it.

`continuity_unproven` is rendered as a work item, never as a pass. The tool's
output must not let an operator mistake a routine emission gap for a clean
result — that is precisely the shape an attacker would hide a fork behind.

## 6. What E2 does not do

- No change to `kb_egress_release_allowed()`; production egress stays closed.
- No consumer registry, delivery receipt, watermark, or budget.
- No automated cross-consumer comparison; the tool is operator-driven.
- No claim that emitted evidence was retained by anyone.

## 7. Validation gates

### Unit / default build

- Cadence fires on both N-records and T-seconds, whichever first.
- Emission renders committed state only; an uncommitted checkpoint is never
  emitted.
- Verifier detects broken witness linkage, a wrong genesis sentinel, a stale
  fence, a revoked signer key, and a bad signature, each typed distinctly.
- The offline tool verifies from bytes alone with no database handle available.

### ASAN / UBSAN

- Emission and verification paths under injected allocation and I/O failure; no
  leak, no use-after-free, no callback after a fail-closed transition.

### Real PG17 on CT103

- Atomicity proven **per ledger** under injected failure at each statement:
  admission, reseal, and D3b open each either commit source-plus-witness together
  or neither.
- `witness_concurrency_exhausted` aborts the source event.
- Two instances racing to sign the same checkpoint sequence: exactly one commits,
  the loser aborts, and no fork is ever persisted. **Verified empirically on
  CT103** (two concurrent `REPEATABLE READ` sessions both computing `max(seq)+1`
  from the same pre-race snapshot, both persisting that seq): the winner commits
  seq 2; the loser fails with `23505` (checkpoint primary key), which the producer
  maps to `DB2_WITNESS_CP_TRANSIENT` and retries on the next cadence tick.

  The mechanism matters and is not what the guard's name suggests. Neither the
  control-row `FOR UPDATE` nor the `P7W05` seq check is what serializes the race:
  the `FOR UPDATE` lock is released at commit without having modified the row, and
  under `REPEATABLE READ` the loser's snapshot still reports `max(seq)=1`, so its
  `v_next` computes to 2 and the `P7W05` equality check **passes**. The unique
  primary key on `seq` is the load-bearing constraint. `P7W05` remains valuable —
  it rejects a producer that signed a stale seq it can see is stale, giving a
  precise error instead of a PK collision — but it is the second line, not the
  first. Both SQLSTATEs map to `TRANSIENT`, so the observable behaviour is correct
  either way; the note exists so nobody later "simplifies away" the primary key on
  the belief that `P7W05` covers the race.
- The checkpoint transaction runs at REPEATABLE READ or stricter; a healthy
  append landing mid-walk does not trip `head_log_mismatch`.
- Checkpoint cross-check catches injected shard-head tampering.
- Boot fails closed on missing, empty, malformed, and revoked-key anchor sets.

### CT260 real daemon

- Real aimee-kb, real PG17, swtpm, and a log/OTLP consumer **configured to
  durably retain the stream** — records as well as checkpoints.
- A locally rewritten chain is caught by comparison against retained copies,
  using the offline tool and nothing else.
- A fork hidden behind a suppressed checkpoint is caught by cross-gap leaf
  comparison; a fork between two emitted checkpoints is caught by the retained
  record stream. Both are exercised, because they are different attacks.

### Canary scan

- Database, files, logs, crash artifacts, and the bytes actually emitted, with
  the `provider:cred` field asserted to be a stable identifier and never a
  credential handle or wrapped-key reference.

## 8. Carried forward from the E1 implementation review (job 11189)

Three items the E1 review confirmed are E2's responsibility, recorded here so they
are not lost between slices:

- **Emission re-encode parity test.** E1 stores logical columns plus the
  server-computed digest, not a wire blob. When E2 re-encodes a record from stored
  columns for emission, a test must assert that the re-encoded wire's digest equals
  the stored `record_hash`. E1's `record_valid` already rejects a record whose
  `is_first_in_shard` disagrees with `shard_seq == 1` or whose witness predecessor
  is not the genesis sentinel on a first record, so a reconstructed-but-inconsistent
  record fails to encode — but the round-trip-from-columns test is E2's to add,
  because E1 has no caller that reconstructs from columns.
- **Typed error for shard-key collision.** `vault_witness_merkle_root` returns a
  generic `-1` for a duplicate 8-byte key (an SMT collision) as well as for other
  invalid input. The checkpoint builder must map a collision to its own typed
  reason, distinct from `checkpoint_shard_ceiling_exceeded`, `checkpoint_deadline_exceeded`,
  and `head_log_mismatch`, so an operator can tell a birthday-bound collision apart
  from tampering or overload.
- **Bounded SERIALIZABLE retry wrapper.** `org_vault_witness_append` is correct in
  single-shot form under the caller's isolation level; it contains no retry. E2 owns
  the bounded (5-attempt) SERIALIZABLE retry around the source-plus-witness
  transaction, raising `witness_concurrency_exhausted` on exhaustion and aborting
  the source event — never committing a source event whose witness row is missing.

## 8b. E2 adversarial review outcome (job 11435)

The review's two CRITICAL findings were both checked against the code and are
**non-issues**. Recorded here with the evidence, because a future reader hitting
the same suspicion should not have to re-derive the answer:

- **Claimed use-after-free of the `hp` text pointer across later blob reads.**
  `aimee_pg_column_text` returns `PQgetvalue(s->result, s->row_index, col)`
  directly. It does *not* go through the one-blob-per-statement cache that
  `aimee_pg_column_blob` uses, so text pointers stay valid for the lifetime of the
  `PGresult` and are unaffected by subsequent blob reads. (The blob cache *is* a
  real hazard — it caused a genuine bug earlier in E2, fixed by copying each
  `bytea` immediately — but it does not extend to text columns.)
- **Claimed the verifier may not recompute and compare predecessor digests, so a
  swapped checkpoint body at the same seq would pass.** It does:
  `vault_witness_verify_checkpoint_run` calls `vault_witness_checkpoint_digest`
  on `checkpoints[i-1]` and feeds the result to
  `vault_witness_checkpoint_continuity` for `checkpoints[i]`. A swapped body has a
  different digest, so the successor's `predecessor_digest` no longer matches and
  the run is flagged. The reviewer asked for explicit confirmation rather than
  asserting the defect; confirmed sound.

Findings that were real and are now fixed:

- The offline parser's item cap allowed a worst-case allocation of roughly 2.4 GB
  from a hostile stream. Lowered so the worst case stays well under a gigabyte;
  the resident input size remains the primary bound.
- The deliberate best-effort `mlock` of the derived signing seed was undocumented
  and read as a swallowed error. It is now commented: `mlock` fails under a low
  `RLIMIT_MEMLOCK` (common in containers), and failing the signature there would
  halt checkpoint production entirely — a worse outcome than a pageable seed that
  is cleansed immediately after use. The cleanse, not the pin, is the load-bearing
  control.

Findings assessed and deliberately not changed:

- SQLSTATE `22023` (`checkpoint_persist: invalid input`) maps to
  `DB2_WITNESS_CP_ERROR` rather than `TRANSIENT`. This is correct: the producer
  constructs those arguments itself, so invalid input is a producer bug that must
  surface, not a condition a retry could clear.

## 9. Deferred

- The full restart and signal-level kill matrix, and only then the release-gate
  flip (E3).
- Automated cross-consumer comparison; it stays an operator procedure.
