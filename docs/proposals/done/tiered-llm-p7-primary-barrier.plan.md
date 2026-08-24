# P7-reseal-b primary vault maintenance barrier

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered.
- **Depends on:** signed-HWM steady-state admission and P7-reseal-a.

## Scope

Add the Postgres-primary control row and complete protected-entrypoint guard that
will quiesce new vault use and mutations before later whole-vault reseal work.
This slice performs no TPM call, changes no TPM generation, stages no wraps, and
exposes no maintenance-start route or operator command.

## Control row and privilege boundary

Add singleton `kb_vault_control` with
`singleton SMALLINT PRIMARY KEY DEFAULT 1 CHECK (singleton = 1)`, `sealed`, positive
`seal_epoch`, `maintenance_kind`, `maintenance_id`, nonnegative `fencing_token`,
and `updated_at`. An unsealed row cannot carry maintenance identity. Seed it
idempotently. Enable RLS with no policy; revoke all table access from PUBLIC and
the runtime role. Because the hardened grant script contains broad schema/table
grants, place targeted control-table and helper revokes after every broad grant so
schema reapplication cannot restore runtime access.

Add internal `org_vault_control_require_open() RETURNS bigint`, VOLATILE,
SECURITY DEFINER, pinned `search_path`, revoked from PUBLIC and not granted to the
runtime role. It first acquires the fixed-domain shared transaction advisory gate,
then selects the singleton `FOR SHARE`, returns `seal_epoch`, and raises
SQLSTATE 55000 with stable `org_vault_control: sealed` classification when the row
is missing, sealed, or in maintenance. The row-share lock is retained until caller
transaction commit and conflicts with the future orchestrator's control-row
`FOR UPDATE` transition.

Add owner/orchestrator-only `org_vault_control_lock_exclusive() RETURNS bigint`,
also revoked from PUBLIC and runtime. It acquires the matching fixed 64-bit
exclusive transaction advisory gate before locking the singleton `FOR UPDATE` and
returns the observed epoch. It mutates nothing. Every later control-row transition
must begin through this helper; direct control-row writes are schema-owner test
fixtures only. Shared-to-exclusive lock upgrade is forbidden. The fixed advisory
key is identical in both function definitions and pinned by schema introspection.

Do not add maintenance begin/clear functions yet: P7-reseal-c/d must atomically
bind the barrier to an operation and WORM intent. Tests may mutate the row only as
schema owner in a throwaway database.

SQLite receives a columns-only schema mirror so generated/schema compatibility
stays intact; it is not an authority or enforcement implementation.

## Admission epoch and error plumbing

Add immutable `seal_epoch` to each `org_vault_key_use_intent`, bind the current
epoch during admission, return it from `org_vault_key_use_admit`, and include it in
the WORM detail. Recreate the Postgres function because its OUT shape changes.
The migration backfills existing immutable intent rows to epoch 1 before
establishing `NOT NULL`, and leaves no default; epoch 1 is the seeded pre-barrier
generation, so historical rows retain their truthful admission generation while
any future insertion path that omits the live epoch fails closed.
Extend the DB2 envelope/result and kb key-use result with the epoch and a typed
SEALED result. Map only the stable SQLSTATE/message classification to SEALED;
other database errors remain database errors.

Sealed state dominates exact request replay. This provides one uniform barrier:
no admission, including replay-without-envelope, succeeds after the barrier. An
open exact replay returns its stored `seal_epoch` even though envelope columns
remain NULL; the C parser requires and preserves that epoch for both new and replay
results rather than parsing it only alongside a newly admitted envelope.
Feeding the database epoch into the local `vault_use_begin` epoch is deferred to
the orchestration writer/synchronization slice.

## Protected-entrypoint inventory and lock order

Every public SECURITY DEFINER function that admits plaintext use or can mutate
vault salts, checks, secrets/current pointers, rotation state, or key-use intent
must call the helper directly after input/auth validation and before any advisory
lock, row lock, or DML. Direct guards remain mandatory on wrappers so the inventory
is mechanically auditable and cannot silently change through nesting.

The initial complete inventory is:

`org_vault_salt_ensure`, `org_vault_kek_check_set`, `org_vault_put`,
`org_vault_delete`, `org_vault_rewrap`, `org_vault_rotation_start`,
`org_vault_rotation_stage`, `org_vault_rotation_transition`,
`org_vault_rotation_finalize`, `org_vault_rotation_claim`,
`org_vault_rotation_heartbeat`, `org_vault_rotation_release`,
`org_vault_rotation_checkpoint_old_ref`, `org_vault_rotation_stage_claimed`,
`org_vault_rotation_probe_admit`, `org_vault_rotation_transition_claimed`,
`org_vault_rotation_fail_claimed`, `org_vault_rotation_remediate`, and
`org_vault_key_use_admit`.

Normative lock order is: input validation; plain MVCC lookup needed for auth;
control advisory gate; control-row lock; per-use/slot advisory lock; row locks;
DML. Claimed
rotation functions that currently lock before auth are changed to plain lookup
and auth, barrier acquisition, then locked re-read. Claimed wrappers must not hold
a rotation-row lock while delegating to a nested function that acquires a slot
advisory lock: they authenticate from a plain snapshot, acquire the barrier, invoke
the advisory-locking operation, and only then lock/re-read for their own mutation.
That locked re-read must revalidate claim owner/token, unexpired lease, and expected
state and raise on mismatch so the nested mutation and WORM append roll back
atomically.
This prevents deadlock with the future orchestrator, which takes control
`FOR UPDATE` before inventory locks.

The explicitly allowed read-only set is `org_vault_salt_read`,
`org_vault_kek_check_read`, `org_vault_get_current`, `org_vault_has`,
`org_vault_list`, `org_vault_list_principals`, `org_vault_current_wraps`,
`org_vault_rotation_authorized`, `org_vault_rotation_get`, and
`org_vault_key_use_candidate`. These return ciphertext or metadata only and
neither admit plaintext use nor mutate authoritative state.

## Validation

- Default server build and schema parity/generation checks.
- Real PG17 schema reapply and full P1 RLS gate.
- SQL gate: singleton defaults and constraints; missing-row, sealed-row, and
  maintenance-identity states each fail closed with the stable classification;
  runtime lacks table/helper privileges; open representative calls work; sealed
  state rejects all 19 entrypoints with 55000 before secret/rotation/intent/WORM
  mutation; read-only ciphertext/metadata calls remain available; admitted intents
  and WORM detail carry the exact epoch.
- Schema introspection pins the exact 19-function protected set, asserts every
  member contains a direct helper call, pins the explicit read-only set, and scans
  direct writers of vault authoritative tables for additions or omissions. It
  also asserts the helper call text precedes every advisory lock, `FOR UPDATE`,
  and authoritative DML token in each protected function definition.
- The two control helpers are the only control functions; introspection pins their
  identical fixed advisory key, ACLs, and advisory-before-row ordering.
- Separate-session concurrency gate: an admitted transaction's `FOR SHARE` blocks
  barrier `FOR UPDATE` until commit; a committed barrier makes racing key-use and
  mutation calls wait then reject without side effects; a many-admitter race has
  no admission committed after barrier commit. Use explicit rendezvous/polling and
  bounded lock timeouts rather than timing-only correctness assumptions.
- Existing P7 rotation and key-use PG gates remain green.

## Deferred

No barrier mutation API, operation row, staging table, wrap inventory/promotion,
TPM call, local writer guard, startup reconciliation, WORM maintenance event,
route, or operator enablement is added. Those belong to P7-reseal-c/d.
