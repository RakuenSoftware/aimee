# P7-reseal-c bounded vault re-wrap staging and promotion

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered and validated on PostgreSQL 17 (CT260), including the
  standalone AES-KW mock driver and concurrency/failure gate.
- **Depends on:** P7-reseal-a prepared TPM2 receipts and P7-reseal-b primary barrier.

## Scope and enablement boundary

Add the Postgres operation, inventory, staging, promotion, fencing, and local WORM
outbox contract required by whole-vault root rotation. Validate it with a
test-only mock driver and real PostgreSQL concurrency/failure injection. This
slice performs no TPM call, changes no custody generation, unseals no production
provider, drains no WORM event externally, and exposes no route, CLI, startup
reconciler, scheduler, or production orchestration caller. The database remains
sealed after promotion.

The only executable caller added here is a standalone test binary. Production
start/resume/reconciliation, receipt verification against the prepared TPM2
provider, local exclusive-use/epoch synchronization, post-promotion cryptographic
verification, completion/unseal, external WORM delivery, kill-boundary testing,
and operator enablement belong to P7-reseal-d.

## Owner-only schema

Add `kb_vault_rewrap_operation`, keyed by a canonical lowercase 32-hex
`operation_id`, with unique `request_id` (1..200 bytes), actor (1..575 bytes),
state, seal epoch, fencing token, old/new nonnegative signed-64-bit generation,
opaque receipt (1..4096 bytes) plus its SHA-256,
inventory/stage counts and SHA-256s, a lowercase class token (never free-form
diagnostic text), and timestamps. Legal states
are:

`preparing -> custody_prepared -> wraps_staged -> reseal_committing -> resealed -> promoted`

`preparing`, `custody_prepared`, or `wraps_staged` may instead become `aborted`.
`reseal_committing`, `resealed`, or `promoted` may become
`recovery_required`; neither terminal state has an outgoing edge. P7-reseal-d
adds verified completion after `promoted`. A constant-key partial unique index
permits exactly one nonterminal operation across all instances. State-changing
functions enforce edges and state-dependent required fields; a state `CHECK`
alone is not treated as an edge guard.

Add `kb_vault_rewrap_dek_stage`, keyed by `(operation_id, source_id)`, with a
second unique key over `(operation_id, principal, agent, cred, version)`. It binds
the stable `org_vault_secret.id`, logical identity, 32-byte source-wrap digest,
and exactly 40-byte new wrap. Every historical and current secret version is in
scope; `org_vault_current` is not the inventory.

Add `kb_vault_rewrap_check_stage`, keyed by `(operation_id, principal)`, for every
`org_vault_salt` row, including a principal with no secret. It binds the source
check digest and a new check that is either exactly 40 bytes or empty when the
source check was empty. Empty verifiers remain empty and are still inventory
members, preventing omission of unused principals.

Add append-only `kb_vault_rewrap_worm` with primary key
`(operation_id,event_kind)` and deterministic `event_id`. Its preimage is the
domain `aimee-vault-rewrap-worm-v1` plus network-order length-prefixed UTF-8
operation ID and event kind; plain concatenation is forbidden. The row binds
operation, seal epoch, fence, state, inventory/stage/receipt digests, actor, and a
content-free detail of at most 1000 bytes. Failure checkpoints may contain only
the operation state plus the bounded lowercase class token; exception text and
operator-supplied diagnostics are forbidden. Triggers reject UPDATE, DELETE, and
TRUNCATE. Begin writes `intent`; resealed, abort, and recovery-required write
their checkpoint/terminal events in the same transaction as the state change.
P7-reseal-d adds completed and the idempotent off-database drain.
`event_id` is the lowercase 64-hex encoding of the raw SHA-256. The outbox
immutability trigger is installed before any transition function can append.

Enable RLS with no policies on all four tables and revoke every table/function
privilege from PUBLIC and `aimee_kb_runtime` after broad grants. Functions are
SECURITY DEFINER, VOLATILE, schema-qualify every object, and pin
`search_path=pg_catalog,public,pg_temp` so a caller's temporary objects cannot
shadow them; they remain executable only by the schema owner/migration-orchestrator
role. There is no runtime or HTTP
grant. Revoke `CREATE` on schema `public` from PUBLIC and runtime and assert no
untrusted role can shadow pinned names. Schema/grant reapplication must preserve
this boundary.

## Canonical inventory

All hashes use PostgreSQL `sha256(bytea)` and a streaming chain so memory is
bounded: `H0 = SHA256(domain)`, `Hi = SHA256(Hi-1 || record)`. Integers use
network-order `int8send`/`int4send`; text is UTF-8 preceded by an unsigned
network-order 32-bit byte length; bytea is likewise length-prefixed. No delimiter
encoding is allowed.

Secret records use domain `aimee-vault-rewrap-secret-inventory-v1`, row tag
`0x53`, and fields `(source_id, principal, agent, cred, version, wrapped_dek)` in
ascending `source_id`. Check records use domain
`aimee-vault-rewrap-check-inventory-v1`, tag `0x56`, and
`(principal,kek_check)` ordered by `principal COLLATE "C"`. The combined inventory
is `SHA256("aimee-vault-rewrap-inventory-v1" || secret_count || secret_hash ||
check_count || check_hash)`, where each count is `int8send` (8 bytes) and every
hash is raw 32-byte SHA-256 output.

The stage digest uses the same orders and encodings under distinct
`aimee-vault-rewrap-secret-stage-v1` and
`aimee-vault-rewrap-check-stage-v1` domains, replaces each raw source value with
`(32-byte source_digest,length-prefixed new value)`, and combines the two chains
as `SHA256("aimee-vault-rewrap-stage-v1" || secret_count || secret_stage_hash ||
check_count || check_stage_hash)`. It is stored only after exact set equality is
proved. Source digests are SHA-256 of the raw source wrap/check, including the
empty check. Tests include non-ASCII and delimiter-like names to pin byte-level
canonicalization.

## Owner-only transaction API

Every mutator first acquires `org_vault_control_lock_exclusive()`; it never calls
the shared helper and never upgrades a lock. It then locks the operation row,
checks `(operation_id,fencing_token)` against the singleton's maintenance ID and
fence, and only then touches source or stage rows. Terminal/exact-replay detection
occurs after taking the exclusive gate but before requiring the live maintenance
identity, so a lost response can be reconciled. Every retry is either an exact
idempotent replay or a typed conflict. A stale driver can never write after a new
fence is installed. Counter increments explicitly reject `BIGINT` exhaustion.
For a transition whose response was lost and whose state was subsequently
advanced, replay succeeds only when every immutable input and stored digest for
that earlier transition still matches; it returns the current later state and
never tries to move backward.

1. `org_vault_rewrap_begin(actor,request_id,operation_id,old_generation,
   new_generation)` requires `new=old+1` and no
   `org_vault_rotation.state <> 'retired'`. It first resolves `request_id`; an
   exact committed replay byte-matches and returns before the clean-singleton
   test. A new request requires a clean unsealed singleton. In one transaction it increments seal epoch and
   fence, installs `sealed=true, maintenance_kind='tpm2-reseal'`, inserts
   `preparing`, and appends the deterministic intent. Exact request replay returns
   the committed operation; conflicting IDs or parameters fail.
2. `org_vault_rewrap_record_prepared(operation,fence,receipt,receipt_sha256)`
   advances only `preparing -> custody_prepared`; exact replay byte-matches. The
   receipt is opaque to SQL, but SQL recomputes `sha256(receipt)` and rejects a
   caller-supplied mismatch. P7-reseal-d must verify it with the provider before
   calling this function.
3. One SERIALIZABLE staging transaction takes the exclusive control gate, rejects
   any weaker `current_setting('transaction_isolation')`, and uses bounded keyset
   pages (requested limit 1..128, no OFFSET) from owner-only inventory
   functions. The mock driver validates old checks, unwraps each DEK under its
   mock old KEK, immediately wraps under its mock new KEK, cleanses the DEK, and
   inserts via exact-idempotent stage functions. It retains only one page and one
   DEK at a time. Any error rolls the whole staging transaction back.
   The one transaction supplies a stable snapshot; the exclusive control gate
   blocks every protected writer for its duration, so keyset pagination cannot
   observe gaps or phantoms. There is no total-row cap: pages continue to
   exhaustion.
4. `org_vault_rewrap_stage_finish` re-inventories the source snapshot, proves
   exact bidirectional set equality for source IDs/logical keys and all salt
   principals, validates every source digest and wrap length, computes both
   canonical digests/counts, and atomically advances
   `custody_prepared -> wraps_staged`. Extra, missing, substituted, or duplicated
   rows fail as integrity errors.
   Stage insertion first resolves the exact live source row/principal and rejects
   an absent identity, any raw-source mismatch, a non-empty new value identical to
   its source, or a second non-identical value. Thus stage cardinality cannot
   exceed inventory cardinality; finish compares raw identities plus source
   digests, not digest equality alone.
5. `org_vault_rewrap_mark_committing` advances only
   `wraps_staged -> reseal_committing`. From that committed point SQL refuses
   abort, because an external commit may already have advanced NV.
   `org_vault_rewrap_mark_resealed` requires the exact stored receipt digest and
   advances only `reseal_committing -> resealed` and appends the deterministic
   resealed checkpoint atomically; it is an owner-only persistence seam, not proof
   that a mock result is a real TPM receipt.
6. `org_vault_rewrap_promote` rejects non-SERIALIZABLE isolation. Under the same
   exclusive control/operation locks it recomputes inventory and exact set
   equality, matches the stored inventory/stage digests, then updates each
   `org_vault_secret` by both stable ID and old raw-wrap digest and each
   `org_vault_salt` by principal and old raw-check digest. Affected row counts must
   equal stored counts. Any mismatch rolls back every update; success atomically
   records `promoted` and leaves the primary barrier sealed. An exact retry checks
   and returns the already-promoted state before comparing old source digests.
7. `org_vault_rewrap_abort` is legal only before `reseal_committing`, deletes
   staging, records `aborted` plus its terminal outbox event, clears maintenance
   identity, restores `sealed=false` because external commit was forbidden before
   `reseal_committing`, and advances the control fence. A terminal replay is
   recognized before comparing the now-advanced live fence.
   `org_vault_rewrap_recovery_required` is legal at or after
   `reseal_committing`, records its terminal event/failure class, advances the
   fence, and retains both sealed barriers. A terminal replay follows the same
   pre-fence ordering. Neither path restores old wraps after
   an external commit could have begun.

The exact read-only owner functions are `org_vault_rewrap_status`,
`org_vault_rewrap_secret_page`, and `org_vault_rewrap_check_page`; the exact
staging mutators are `org_vault_rewrap_stage_dek` and
`org_vault_rewrap_stage_check`. All five are explicitly revoked from PUBLIC and
runtime alongside every transition function; no default EXECUTE remains.
Read-only owner functions expose bounded operation/status and inventory pages.
They reveal wrapped material only to the owner-only test/orchestration role and
never to runtime. They do not weaken the P7-reseal-b public read-only inventory.

## Validation

- Fresh and in-place schema application twice; hardened grant reapplication;
  introspection pins exact tables, constraints/indexes/triggers, helper security
  attributes, and absence of runtime/PUBLIC access.
- A source/link inventory gate proves the mock driver is referenced only by a
  non-installed test target and that no production object, route, startup hook,
  scheduler, or new code in this slice references a TPM reseal symbol.
- Real PostgreSQL 17 state/edge and exact-replay tests, including stale fences,
  concurrent begin (one winner), illegal abort after committing, receipt mismatch,
  and terminal immutability.
- A test-only mock driver stages more than 512 retained secret versions plus salt
  principals with zero secrets, using bounded pages and real AES-KW unwrap/wrap.
  It proves every ciphertext remains byte-identical, all DEK wraps and established
  checks change while empty checks remain empty, old KEK unwrap fails after
  promotion, and new KEK unwrap succeeds.
- Failure injection for wrong old KEK, corrupt wrap/check, omitted/extra/substituted
  stage row, source mutation between staging and promotion, forced disconnect
  during promotion, and serialization retry. A forced disconnect is treated as an
  in-doubt outcome: reconnect must observe either unchanged `resealed` source data
  or exact committed `promoted`, never a partial update. Each case proves the
  barrier remains sealed.
- Separate-session rendezvous proves a pre-barrier protected mutation drains before
  begin commits, all later protected calls reject, two stale staging drivers are
  fenced, and promotion cannot race an authoritative writer.
- Lock-cycle tests include slot mutation, claimed rotation, begin, stage-finish,
  abort, recovery-required, and promotion; all respect control advisory gate ->
  control row -> operation row -> source/stage rows, with no shared-to-exclusive
  upgrade.
- Default build/unit tests plus ASAN/UBSAN for the mock driver; canary scans cover
  database dumps, test logs, and process artifacts for raw KEK/DEK bytes.
- The full real-PG P1/P7 regression gate remains green.

## Deferred to P7-reseal-d

No production driver calls these functions. P7-reseal-d owns the local exclusive
use guard, primary/local epoch synchronization, fresh KEK generation in protected
memory, prepared-TPM2 `prepare/recover_kek/status/commit/cleanup`, receipt and
installed-artifact verification, restart reconciliation, post-promotion unwrap
verification, `completed` transition, continuation cleanup, external WORM drain,
kill-after-every-boundary matrix, startup fail-closed behavior, management API,
operator CLI, scheduling, and the only enablement switch.
