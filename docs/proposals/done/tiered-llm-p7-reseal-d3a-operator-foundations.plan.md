# P7-reseal-d3a operator discovery and activation foundations

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** implemented; production-uninvoked until D3b lands.
- **Depends on:** P7-reseal D1, D2a, and D2b.
- **Enables:** D3b's protected-secret START/RESUME, terminal cleanup, and explicit
  operational unseal.

## Why D3 is split again

D2b deliberately ends every whole-vault TPM2 reconciliation sealed. It has no
production caller, active-operation discovery, dedicated database authority, secure
operator transport, single-runtime proof, prepared-artifact cleanup, or safe
completed-to-open transition. Exposing START/RESUME before all of those exist can
complete a rotation and leave the deployment unavailable, or race a different kb
process whose readers are outside D2b's process-local maintenance guard.

D3a therefore adds only the load-bearing authority, discovery, singleton, startup,
and local status foundations. It does **not** accept an operator secret and cannot
call the reconciler. D3b adds the secret-bearing mutations only after their entire
cleanup/open/restart protocol can be reviewed together. This is a fail-closed
delivery split, not a claim that operator enablement is complete after D3a.

The existing D2b seams rotate an already-provisioned TPM2-custodied root. They do
not migrate a file-custodied vault to TPM2 and do not create a generic durable
`protection_enabled` bit. D3 must never describe this as file-to-TPM migration.

## 1. Dedicated orchestration database authority

Create the non-login capability role `aimee_kb_vault_orchestrator` and the dedicated
NOINHERIT LOGIN authenticator `aimee_kb_vault_orchestrator_login`. The authenticator
has only database CONNECT plus membership permitting explicit
`SET ROLE aimee_kb_vault_orchestrator`; before the switch it has no schema, table,
sequence, or function authority. Revoke all rewrap tables,
sequences, and functions from PUBLIC and `aimee_kb_runtime`, then grant this role
only the exact D3 discovery functions in this slice. Keep the existing owner-only
D2 mutation functions ungranted until D3b enumerates their complete executable
closure. `schema_grants.sql` and the PG17 grant gate must assert the positive and
negative ACLs by signature; no role inherits table DML.

Add a distinct, bounded orchestration connection seam configured only when the D3
operator surface is enabled. It must not reuse `db2_pool`, the ordinary runtime
credential, a read replica, or caller-controlled search paths. Configuration is
all-or-none and environment-backed: operator listener activation plus the dedicated
primary PostgreSQL URL. Every fresh connection verifies the exact LOGIN session
user and its NOINHERIT/non-superuser/non-BYPASSRLS/non-owner posture, performs the
fixed `SET ROLE`, then verifies the exact non-login current user before any status
query. It resets/discards rather than returning a connection with ambiguous role
state. The connection verifies TLS policy through the normal PostgreSQL URL
handling, pins statement timeouts, and never falls back to the runtime pool. Tests
inject a connection vtable; production code owns no global test rebinding.
Connection, role setup, BEGIN/query/COMMIT/ROLLBACK, and result drain use libpq's
nonblocking send/consume API behind a deadline-aware database wrapper. TCP accepts
only `verify-full` with either a numeric host or a DNS certificate identity plus a
separate numeric `hostaddr`; this prevents synchronous DNS from escaping the
deadline. Local Unix sockets remain valid. Every poll shares one monotonic
whole-operation deadline capped at 2 seconds; the query reserves its final 100ms
for PostgreSQL 17's asynchronous best-effort cancel, then closes/discards the
connection and returns typed unavailable.
No blocking libpq query call is permitted, `connect_timeout` is capped at 2 seconds,
and shutdown may close the owned connection to wake a poll before listener join.
The v1 seam owns one non-pooled connection: any SQL, protocol, transaction-state,
role, GUC, timeout, or decoding error discards it. Before every new status
transaction it re-verifies `session_user`, `current_user`, idle transaction state,
`search_path`, row security, and statement/lock timeouts; no connection is returned
to another subsystem. One mutex owns the connection across role verification and
the whole transaction; no parallel caller can issue `RESET ROLE` or otherwise race
session state.

Add owner/orchestrator-only SECURITY DEFINER functions with pinned
`search_path=pg_catalog`:

- `aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()` returns the
  control tuple and the single
  relevant active or highest-fence terminal operation without receipts, digests,
  wrapped material, or free-form database errors.

Active-operation and actor/request-id dispatch discovery are not consumed by D3a,
so their functions and grants are deferred to D3b. The D3a capability role receives
exactly one effectively invocable application function:
`aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()`; the capability
has no `public` schema usage.

All functions acquire the existing shared control advisory gate and row lock so a
database snapshot cannot splice control and operation generations across a
transition. They
validate canonical actor/request bounds and fail on duplicate/impossible rows.
Typed C decoders accept only the closed state set, exact nullable shape, positive
epochs/fences/generations, and canonical lowercase failure classes; every malformed
or surplus row is integrity failure.

## 2. Stable secret-free discovery model

Expose one internal status projection with these closed states:

- `unsupported`: non-Linux/non-TPM build or D3 unavailable;
- `disabled`: D3 operator surface is not configured;
- `sealed_idle`: TPM2 custody selected, primary sealed, no active operation;
- `operational`: primary open, local provider unsealed, no operation;
- `local_unseal_required`: primary open but this process has no unsealed provider;
- `resume_required`: a recoverable nonterminal operation exists;
- `recovery_required`: durable quarantine exists;
- `completed_sealed`: an exact completed operation awaits D3b cleanup/unseal;
- `backend_unavailable`: configured local TPM2 status cannot be established; and
- `integrity_failure`: configuration/control/operation shapes disagree.

`unsupported` and `disabled` are internal configuration classifications, not
remotely observable claims. Startup has three explicit branches:

1. **D3 disabled:** a TPM2-custody daemon still takes the mandatory singleton lock,
   then follows the existing sealed startup path. It creates no orchestration
   connection or STATUS listener; the client therefore returns transport exit 4.
   A non-TPM daemon is unchanged.
2. **D3 configured but ineligible:** a non-Linux/non-TPM build, non-TPM custody,
   remote TCTI, incomplete fixed-fd/database configuration, or failed singleton
   check is a startup configuration error. No listener is activated.
3. **D3 enabled and eligible:** only this branch executes the dedicated connection,
   two-snapshot status, sealed-safe epoch initialization, and STATUS listener below.
   A successful v1 STATUS response therefore never emits `unsupported` or
   `disabled`; their numeric values are reserved for a future protocol version.

The projection includes only build support, configured custody class, stable state,
operation ID when one exists, closed operation state, seal epoch, fencing token,
old/new generations, and sanitized remediation code. It never returns or logs the
TPM TCTI, blob/lock paths, NV handle, receipt/digests, provider error prose,
credentials, secret lengths, or key material.

Provider-local status must be queried through a new read-only selected-provider
seam, not inferred from configuration. D3a may distinguish supported/sealed/local
unsealed/backend unavailable but must not perform TPM I/O, authentication,
discovery, unseal, cleanup, or repair. The seam reads only the initialized provider
object and process-local seal/cache state under a deadline-aware mutex acquisition
capped at 50ms. A missing/failed context or mutex deadline is `unavailable`; an
impossible local state is `malformed`. D3a does not claim that a sealed external TPM
is reachable. Live backend probing and wrong-secret classification belong to D3b.

The complete status read is a bounded two-snapshot protocol: read authoritative
control plus relevant operation in one database transaction, query provider-local
state without holding a database transaction across TPM access, then re-read the
complete database tuple. Accept only byte-identical control fields and operation
presence/id/state/epoch/fence/generations/failure class. Retry at most three times;
continued motion returns `integrity_failure` for operator status and fails startup.
Every D2/D3 mutation already takes the conflicting exclusive control gate.

Reserve durable relevance now rather than treating any historical completion as
pending forever. Add `last_opened_rewrap_fence BIGINT NOT NULL DEFAULT 0` to
`kb_vault_control`, bounded by the current fencing token. Make operation fencing
tokens unique and define canonical recency as the greatest fencing token, never a
timestamp. D3b's atomic open edge will set `last_opened_rewrap_fence` to the exact
completed operation fence. Until then it remains zero.

A nonzero marker is valid only if exactly one still-present operation row has that
fencing token and its state is `completed`; zero references no row. The marker is
the most recently opened completion, not necessarily the newest operation: a later
outstanding recovery obligation is valid and remains visible above the marker.
Discovery
rejects a nonexistent marker, a marker targeting active/aborted/recovery state,
duplicate or regressing fences, or a marker greater than the durable control fence.
The unique active operation is relevant only when no outstanding terminal
obligation exists. An outstanding obligation is any `completed` or
`recovery_required` operation above the opened marker. Exactly zero or one is
permitted. If an obligation exists, no greater-fence row of any state may exist;
newer aborted/active/completed/recovery history is `integrity_failure`, not a way to
hide the older obligation. `completed_sealed` requires the one outstanding row to
be `completed` and durable control sealed; `recovery_required` requires it to be
that state and always dominates historical rows. Aborted rows are historical only
when no obligation exists. Multiple active rows, an active row at/below the opened
marker, or any other unlisted history is `integrity_failure`.

The schema migration takes the exclusive control gate before adding/backfilling the
marker and unique fence constraint. It rejects a wrong-type/NULL partial marker,
normalizes a safe BIGINT marker's default/nullability, and catalog-validates any
same-named fence index rather than trusting `IF NOT EXISTS`. Empty or aborted-only history receives zero.
Sealed completed/recovery history remains unacknowledged at zero. Because the
pre-D3 schema has no completed-to-open function, open control plus any completed or
recovery history is not guessed: migration fails with a stable remediation code.
Likewise it fails on completed-then-aborted, recovery-then-any-later-row, duplicate
fences, or malformed control history. The PG17 upgrade gate covers every one of
these cases and preserves the exact prior schema on a failed transaction.

The normative projection order is:

| Durable/configuration tuple | provider-local result | status |
|---|---|---|
| build/provider not supported | any | `unsupported` |
| D3 status surface not configured | any | `disabled` |
| malformed/missing/duplicate/regressing tuple | any | `integrity_failure` |
| exact active op + matching sealed maintenance control | `available_sealed`/`available_unsealed`/`unavailable` | `resume_required` |
| sole outstanding terminal is `recovery_required` + sealed control | `available_sealed`/`available_unsealed`/`unavailable` | `recovery_required` |
| sole outstanding terminal is `completed` + sealed control | `available_sealed` | `completed_sealed` |
| no relevant op + sealed idle control | `available_sealed` | `sealed_idle` |
| no relevant op + open control | `unavailable` | `backend_unavailable` |
| no relevant op + open control | `available_sealed` | `local_unseal_required` |
| no relevant op + open control | `available_unsealed` | `operational` |

Any unlisted combination is `integrity_failure`. Durable active/quarantine states
remain visible even if the provider is temporarily unavailable; their remediation
code may say `backend_unavailable`, but the stable state does not erase the durable
recovery obligation. A provider error during sealed-idle status is
`backend_unavailable`, not `sealed_idle`.
The provider seam has exactly four results: `available_sealed`,
`available_unsealed`, `unavailable`, and `malformed`. `malformed` always maps to
`integrity_failure`; only typed `unavailable` may be dominated by a durable active
or recovery obligation as shown above.

## 3. Proven pinned single-runtime eligibility

D2b's local guard is safe only for the documented single active kb runtime on one
locally pinned TPM. Every production kb process selecting TPM2 custody, whether or
not D3 or its operator listener is configured, must acquire a daemon-lifetime
nonblocking lock before custody selection, primary-epoch initialization, and every
public or operator listener. A D3-disabled process and D3-enabled process therefore
conflict exactly like two D3-enabled processes.
The lock identity is the canonical TPM NV index, stored in a fixed service-owned
runtime-lock directory. Open every directory component without symlink traversal;
the directory and file must have the expected owner and restrictive mode. Hold the
fd with `O_CLOEXEC` until all workers/listeners have joined and DB2 has shut down;
explicitly close it in every fork/exec helper child. A second process for the same
NV index refuses startup.
An opaque singleton-lock owner is the only code that knows the fd. It verifies the
fd is open, still names the expected regular lock inode, and has `FD_CLOEXEC` before
custody initialization and again before listener activation; generic cleanup and
helper launch code cannot access or rebind it. After every repository helper-launch
site it repeats the fd/inode/CLOEXEC checks and idempotently calls
`flock(fd, LOCK_EX|LOCK_NB)` before continuing. The lock is acquired on exactly one
`O_CLOEXEC|O_NONBLOCK` fd. Forked duplicates share the open-file-description lock;
a child close does not release the parent's held lock, while CLOEXEC prevents an
exec survivor from prolonging ownership. Runtime health polling is deferred
because no D3a mutation exists, but D3b must add pre-mutation verification.

D3 eligibility accepts only the native local TPM resource-manager TCTI in
production. Arbitrary TCP, `mssim`, and `swtpm` TCTIs fail closed before listener
activation. A compile-time integration-only test override may admit loopback swtpm;
the build must emit and append the existing primary-backed local `kb_audit_event`
hash-chain record before serving, failing startup if that durable append fails.
This is a build-integrity admission record, not a claim of external WORM delivery.
The override exists only under `AIMEE_P7_D3_INTEGRATION_TEST_OVERRIDE`; production
build-integrity proves the symbol/path is absent. No production
configuration knob can enable a remote TPM.

This scoped flock proves only the supported single-host, single-runtime TPM
deployment. It is not represented as a distributed lease and does not enable TPM
reseal in a horizontally scaled kb fleet. KMS/PKCS#11 fleet activation remains
deferred.

The mandatory lock is a rollout prerequisite, not merely a D3 feature. D3b cannot
be activated in a mixed-version deployment: the operator must stop every possible
kb runtime, upgrade all launch targets to the mandatory-lock build, and restart the
single pinned runtime before enabling mutation opcodes. D3a tests the mixed
D3-disabled/D3-enabled pair; the deployment docs record the stop-the-world upgrade
gate. If the service manager cannot prove a single launch target, D3b stays disabled.

## 4. In-process local operator status service

Add a dedicated AF_UNIX listener owned by the running aimee-kb process. It is
available while the vault is sealed so an operator can diagnose recovery. The
listener is supplied as a pre-opened, root-created socket fd; the daemon never
binds an arbitrary path from config. Startup validates AF_UNIX/SOCK_STREAM,
listening state and filesystem pathname (not abstract namespace). Provenance is
the trusted service-manager fixed inherited-fd contract, not listener
`SO_PEERCRED`: the service invocation names one fixed fd and D3 refuses an
environment-selected arbitrary descriptor. `fstat` proves a socket fd;
`getsockname` obtains the pathname; independent `lstat` proves that pathname is a
socket with exact root owner and 0600 mode beneath the fixed root-owned,
non-writable runtime directory. Sockfs and filesystem inode numbers are not
compared. Every accepted connection validates Linux `SO_PEERCRED` uid 0; version 1
supports root operators only. Failure closes before a request byte is parsed.

D3a defines protocol version 1 with one operation, `STATUS`. One connection carries
exactly one request and one response, then closes. Integers are unsigned big-endian.
Read-header and write-response deadlines are each 5 seconds.

Request is exactly 16 bytes: magic `A7VS` (4), version `u16=1`, opcode
`u16` (`1=STATUS`; 2..4 reserved for START/RESUME/UNSEAL), payload length
`u32=0`, reserved `u32=0`. No request may exceed 16 bytes; any readable surplus is
a bad frame. A truncated/bad-magic/bad-version/bad-reserved frame closes without a
response. A well-formed unknown/reserved opcode receives a protocol-error response
and no vault payload.

Response header is exactly 16 bytes: magic `A7VR`, version `u16=1`, transport result
`u16` (`0=ok`, `1=bad_frame`, `2=unsupported_opcode`, `3=internal`), payload length
`u32` (`80` only for ok, else `0`), reserved `u32=0`. The 80-byte STATUS payload is:
vault state `u16` (1 unsupported-reserved, 2 disabled-reserved, 3 sealed_idle, 4 operational,
5 local_unseal_required, 6 resume_required, 7 recovery_required,
8 completed_sealed, 9 backend_unavailable, 10 integrity_failure), operation state
`u16` (0 none, 1 preparing, 2 custody_prepared, 3 wraps_staged,
4 reseal_committing, 5 resealed, 6 promoted, 7 completed, 8 aborted,
9 recovery_required), remediation `u16` (0 none, 1 configure, 2 unseal, 3 resume,
4 recover, 5 upgrade, 6 backend, 7 integrity, 8 finalize), flags `u16` (bit 0 operation-present;
all other bits zero), then five `u64` values (seal epoch, control fence, old
generation, new generation, last-opened fence), 16 raw operation-id bytes (all zero
when absent), and 16 zero reserved bytes. Values that do not fit their signed
database domain are integrity failures. Transport `unsupported_opcode` is therefore
unambiguous from a successful STATUS whose vault state is `unsupported`.
All nine operation-state values are valid D3a observations of durable D2 history;
in particular `promoted=6` is not a vault-status alias and must never be decoded as
`completed=7`. The vault-state field is authoritative for operator action, while
operation-state reports the exact durable row.
For an absent operation, operation state, flags, old generation, new generation,
and operation ID are all zero. Every enabled STATUS has a positive seal epoch and
control fence plus a nonnegative last-opened fence. Reserved unsupported/disabled
responses, if a future version permits them, must zero every tuple field.

Remediation is deterministic: `sealed_idle` and `local_unseal_required` use
`2=unseal`; `operational` uses 0; `resume_required` uses `3=resume` unless provider
state is unavailable, then `6=backend`; `recovery_required` uses `4=recover` unless
provider state is unavailable, then `6=backend`; `completed_sealed` uses
`8=finalize`; `backend_unavailable` uses 6; and `integrity_failure` uses 7.

There is no HTTP, JSON, bearer, environment-supplied actor, or network listener.
Unknown opcodes are effectless. Unauthorized peers are closed without response.

Add `aimee-kb vault status [--json]` as the companion client. Version 1 uses only
the fixed trusted socket pathname and has no path override. Before connect it
`lstat`s the fixed path and trusted parent, requiring no symlink, a root-owned 0600
socket, and root-owned 0700 parent. After connect, client-side `SO_PEERCRED` on the
connected socket must identify the root daemon; independently, the server validates
uid 0 on its accepted socket. The client emits exit 0 only for `operational`,
2 for `sealed_idle` or another valid action-required/backend state,
3 for `integrity_failure`, and 4 for transport/protocol/permission failure. It never falls back to
the kb HTTP port. There is no `--secret`, START, RESUME, cleanup, unseal, or mutable
verb in D3a.

The listener owns a joined worker lifecycle. Shutdown first closes admission,
wakes accept, drains all status callbacks, joins the thread, then releases the
runtime lock. Initialization failure unwinds all fds and leaves no listener.

## 5. Fail-closed startup reconciliation

In the D3-enabled-and-eligible branch only, use this exact startup order before
general listeners. The disabled and ineligible branches follow the explicit rules
in §2 and do not enter these steps:

1. Parse and validate local TPM2 configuration, then acquire the mandatory
   daemon-lifetime NV-index lock before initializing custody.
2. Select TPM2 custody and immediately force seal/clear local admission; failure is
   fatal.
3. Create and verify the dedicated orchestration connection.
4. Obtain one complete bounded two-snapshot database/provider status while still
   sealed.
5. Run the existing primary epoch initialization only through its sealed-safe path.
6. Invoke a new, independent complete two-snapshot status read after epoch
   initialization. `vault_primary_epoch_initialize` changes only process-local
   guard state and performs no database transition, so each read still requires a
   byte-identical database tuple. Any
   tuple change or classification disagreement fails startup.
7. Activate the STATUS listener and enumerated non-secret surfaces. Only an exact
   `operational` result may activate org-key egress; every other state keeps it
   unavailable.

Never auto-resume: the TPM authorization secret is intentionally not persisted.
D3a deliberately cannot produce `operational` during a clean startup because step
2 forces the local TPM provider sealed and D3a has no unseal opcode. `operational`
is reserved for a running process after D3b's explicit unseal flow. Durable
`resume_required` and `recovery_required` at startup are legitimate sealed states:
they activate only non-secret APIs plus STATUS, never egress or automatic mutation.

An impossible control/operation combination, missing singleton, malformed state,
dedicated-authority failure, singleton-lock loss, `backend_unavailable`, or
`integrity_failure` is fatal before public serving. `sealed_idle`,
`local_unseal_required`, `resume_required`, `recovery_required`, and
`completed_sealed` may serve only the already-enumerated non-secret APIs and local
STATUS; none activates egress. `local_unseal_required` is not silently called
operational. Ordinary key use remains fail closed through the existing primary
admission and local custody checks.

## 6. Validation

- Unit/default: strict status decoder; every control × operation × local-provider
  combination; duplicate/surplus/malformed rows; closed state/exit mappings;
  non-TPM unsupported with no DB mutation.
- PostgreSQL 17: current and legacy migrations; concurrent discovery versus every
  D2b transition; request replay; unique-active corruption; exact ACL/grant
  enumeration; runtime and authenticator roles denied every D3 function and table
  before SET ROLE; orchestrator role denied all D2 mutations; marker targets
  nonexistent/aborted/recovery/active/duplicate/zero/valid-completed fences; and
  transactional migration cases for empty, sealed-completed, open-historical,
  recovery, completed-then-aborted, and malformed histories.
- ACL assertions enumerate every exact table privilege and executable function
  signature from `information_schema.role_table_grants` and `aclexplode`; tests
  prove only the capability role has the listed discovery EXECUTEs after SET ROLE.
- UDS: wrong inherited fd/family/type/path/mode/owner/trusted-parent/peer uid,
  abstract socket, symlink, truncation, immediately readable surplus bytes,
  slowloris deadlines, unknown opcode, disconnects, shutdown/restart, occupied fd,
  and fd reuse. Unauthorized peers receive no vault topology.
- Runtime singleton: two real kb processes for the same NV index; exactly one
  reaches epoch initialization/listener activation. Lock is retained through
  shutdown drain and released after clean exit; crash releases it. Distinct NV
  indexes do not conflict. Repeat with one D3-disabled and one D3-enabled process,
  and with a fork/exec helper that must not inherit the lock fd.
- CT260: exact-head TPM2 build with loopback-swtpm test override and real PG17;
  exercise sealed idle, active preparing/custody-prepared resume-required,
  completed-sealed, recovery-required, missing local provider context, bounded
  local-status mutex timeout, process restart, and second process refusal. Live TPM
  reachability probing remains D3b. Scan output/logs for TCTI, paths,
  receipt/digests, and canaries.
- ASAN/UBSAN/LSAN and bounded frame fuzzing; server and kb production builds, lint,
  build-integrity, schema grant gate, and existing D1/D2b matrices remain green.

## Explicit D3b boundary

D3b owns active/request-id dispatch discovery, protected secret ingress,
peer-derived actor and stable request-id dispatch, CSPRNG operation IDs,
START/RESUME, exact terminal artifact cleanup,
an owner-only atomic completed-to-open transition with immutable outbox event,
committed epoch synchronization, a maintenance-guard finish path that can safely
retain an unsealed provider, explicit operational unseal after restart, typed
wrong-secret/backend errors, and the kill-after-every-boundary matrix.

External WORM delivery/acknowledgement, automatic scheduling, recovery quorum or
break-glass ceremony, file-to-TPM migration, remote TPM, KMS/PKCS#11 fleet root
activation, and destructive re-provisioning remain separate slices.
