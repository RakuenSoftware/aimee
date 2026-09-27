# Governed action receipts

The Server's native tool dispatch boundary records host-issued intents after
mechanical argument rewrites. The Go tools owner resolves exact local file paths
and canonical arguments. The existing Go execution-policy owner rechecks the
operator policy, tool, destination, payload and request-byte reservation inside
a principal-owned DB1 transaction. Only a committed `dispatching` transition
releases one dispatch permission.

The initial adapters cover `read_file`, `write_file`, `edit_file` and
`edit_symbol` on a host-accessible filesystem. `write_file` has exact readback
verification; edits retain acknowledgement until an exact verifier is available.
A namespaced external tool or Git publication without an exact resource adapter
is refused by this native governed boundary. Other tool families and independent
third-party CLI execution are outside this adapter set. Detached filesystems
without host resource resolution fail closed rather than inventing a destination.

## Evidence and admission

When the host retained memory, admission requires the Go memory owner's exact
source commitment and acknowledged provider receipt. It acquires the existing
source mutation guards, revalidates current source versions, and retains those
guards through the DB1 dispatch commit. The memory observation has a one-second
freshness window; the existing committed guards require explicit completion and
do not silently expire into a permission. Source mutations committed before
admission block stale use. Mutations after admission cannot undo an effect.

An explicitly memory-free file call reports no retained memory coverage. An
operator-declared publication path additionally requires a current retained
memory handle. A missing required handle or lost owner process refuses admission.
Operator-policy snapshots are checked at admission; this is not a cross-system
atomic snapshot of mutable filesystem policy and PostgreSQL.

## Idempotency and truthfulness

Intents bind principal, session lineage, request and attempt IDs, tool class,
canonical destination, exact payload digest, purpose, source commitment, provider
receipt, policy generation, revocation/source generation and expiry. A repeated
key with changed material conflicts. A duplicate committed dispatch cannot execute
again. An unresolved destination blocks a new attempt from bypassing reconciliation.

States are `prepared`, `admitted`, `dispatching`, `acknowledged`,
`effect_confirmed`, `failed_before_effect` and `outcome_unknown`. A crash after the
dispatch commit is conservatively unknown. Acknowledgement does not prove a
postcondition. File verification checks the original approved path and bytes,
rejects redirection, and reports an observed object version; it does not promise
that another actor cannot subsequently change that file.

Raw arguments and file contents travel transiently to the resource owner and
are not stored in the action journal. The journal retains commitments, destination
identity, bounded observations and an ordered audit. It survives prompt resets and
handler restarts. It is bounded to 1,024 actions, 4,096 audit transitions and 4 MiB
per root; reaching a limit refuses further admission without deleting history.

## Operator composition policy

The existing `policy.json` or `.aimee-policy.json` accepts an `actions` object:

```json
{
  "actions": {
    "max_calls": 100,
    "max_work_units": 262144,
    "sensitive_path_prefixes": ["/workspace/private"],
    "published_path_prefixes": ["/workspace/public"],
    "forbid_sensitive_publish": true,
    "forbid_elevated_use": true
  }
}
```

Prefixes are canonical absolute host paths. They classify actual resource
operations, ignoring model-authored `side_effect` labels. Work units are canonical
request bytes, not billing cost. Missing ceilings are unlimited; explicit zero
blocks reservations. Sensitive-read and publication composition applies to the
registered file adapters. Privilege sequence checks are implemented in the owner
reducer; no privilege-changing resource adapter is currently registered.

Reservations and composition state are serialized with admission. Failed and
unknown attempts retain their spending. Delegates using the same authenticated
session share its root. The private host-only `fork` operation binds an unused
child session to its authenticated parent's root; an existing child cannot be
reparented. Native task binding refreshes preserve copied host parent bindings
through this operation before replacing them. This contract does not infer lineage for unrelated external sessions.

## Inspection and reconciliation

```sh
aimee action receipt inspect --session_id SESSION --action_id ACTION --json
aimee action receipt reconcile --session_id SESSION --action_id ACTION \
  --directory /workspace \
  --arguments_json '{"path":"/workspace/result.txt","content":"approved bytes"}' --json
```

`cancel` stops a prepared or admitted action without refunding reservations.
A dispatching or unknown effect requires reconciliation; cancellation cannot
relabel it as unsent.

The HTTP equivalent is `POST /v1/action/receipt`. Both require tool-execution
capability and ownership of the session lineage. Reconciliation asks the resource
owner to verify the original intent. A caller cannot supply a success state or
object version. Unsupported or unsuccessful verification leaves the durable
receipt unchanged and never authorizes blind replay.

Required freshness and receipt checks have no off/shadow toggle. Removing an
optional composition ceiling does not erase spent reservations or allow reuse of
an old dispatch. Do not remove the additive action schema when rolling back a
candidate; the journal is needed to reconcile actions from that candidate.

Rollback to a binary without this admission boundary requires quiescing governed
action traffic. Once effects have been admitted, retain their journal and
reconcile them; restoring a pre-effect database snapshot is not reconciliation.
