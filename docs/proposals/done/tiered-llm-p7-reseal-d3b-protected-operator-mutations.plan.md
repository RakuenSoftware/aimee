# P7-reseal-d3b protected operator mutations and operational unseal

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** implemented and validated; merged as the P7-D3b delivery slice.
- **Depends on:** P7-reseal D1, D2a, D2b, and D3a.
- **Enables:** authorized whole-vault START/RESUME, exact terminal artifact
  cleanup, completed-to-open finalization, and explicit operational unseal for
  the pinned single-runtime TPM2 deployment.

## Scope and release boundary

D3a deliberately exposes only secret-free status. D3b activates the three
reserved local operator opcodes only after their database authority, protected
secret ingress, crash-safe cleanup/open choreography, and transition into normal
kb serving can be reviewed as one protocol.

This slice supports only the D3a-qualified topology: one local Linux aimee-kb
runtime holding the mandatory daemon-lifetime lock for one canonical TPM2 NV
index and using the native local resource-manager TCTI. The integration-only
loopback swtpm build override remains acceptable only on CT260 and remains
durably audited before serving. D3b does not turn the local singleton into a
distributed lease and does not make TPM2 reseal safe for horizontally scaled kb
runtimes.

The operator can:

1. explicitly unseal an idle or restart-sealed vault;
2. start one whole-vault reseal using a stable request ID;
3. resume the one exact durable active operation;
4. finalize an exact completed operation by cleaning its continuation artifact,
   proving the installed KEK, atomically opening durable control, and retaining
   the provider unsealed; and
5. observe a stable, secret-free result and status after every request.

No operation clears `recovery_required`. Quarantine is report-only until a
separately reviewed recovery ceremony exists.

## Non-negotiable invariants

- The ordinary `aimee_kb_runtime` connection pool receives no rewrap mutation
  authority. Every D3 mutation uses the dedicated D3a LOGIN -> explicit
  `SET ROLE aimee_kb_vault_orchestrator` connection and its exact capability
  closure.
- The TPM authorization secret is never accepted from argv, environment, HTTP,
  JSON, logs, database rows, or heap-owned request objects. It exists only in a
  bounded protected arena and provider calls made while that arena is owned.
- Actor identity is derived from the authenticated Unix peer. Version 1 accepts
  root only and maps it to one canonical actor; no frame field may override it.
  Every dispatch and replay edge compares that exact canonical actor with the
  actor stored beside the request ID, operation ID, and generations. Because all
  accepted v1 peers canonicalize to uid 0, D3b makes no MAC, channel-binding, or
  anti-spoof claim between multiple root operators; adding a MAC would not create
  a distinct v1 principal and is not part of this protocol.
- A START request ID resolves to at most one immutable operation identity. Exact
  replay resumes that operation; changed actor, operation identity, generation,
  or request metadata raises one stable replay-mismatch SQLSTATE and is integrity
  failure, never a second START.
- START reservation and exact-replay lookup are one database edge backed by the
  unique request constraint. Competing STARTs use `INSERT ... ON CONFLICT DO
  NOTHING` followed by a locked exact lookup, so they cannot both create logical
  operations.
- RESUME adopts only the sole authoritative active operation returned by the
  private discovery function. It never accepts an arbitrary operation ID from a
  client.
- Every mutation revalidates the opaque daemon-lifetime TPM singleton before its
  first effect and again before reporting or publishing an operational state.
  Its live preflight also verifies the canonical NV identity, counter attributes,
  current PolicyNV generation binding, and the held nonblocking write lock.
- No database transaction spans TPM, provider, RNG, guarded-unseal, filesystem
  cleanup, or protected-key verification. Local AES-KW/digest verification under
  an already-owned protected KEK may occur in a bounded database transaction.
- D2b continues to end sealed. Only D3b's exact open protocol may release a
  maintenance guard while the provider remains unsealed.
- Prepared continuation artifacts are removed only after durable `completed`
  authorization represented by the existing typed terminal-only cleanup enum.
  No boolean, state string, or caller-provided integer can authorize cleanup.
  Cleanup precedes durable open, is idempotent, fsyncs the parent directory, and
  leaves `completed + CLEANED` resumable after a crash.
- Durable control is opened only after the installed active KEK is unsealed and
  bound to the completed receipt plus every retained nonempty current KEK check.
- The local use epoch is synchronized to the committed open epoch while the
  exclusive maintenance guard is still held. No admitted key use can cross the
  sealed-to-open edge with a stale local epoch.
- Any uncertain open commit, singleton failure, guard-finish failure, malformed
  result, or impossible status attempts a fail-closed local seal. A later status
  must classify the durable result; the process never guesses whether open
  committed.
- Neither successful local outbox append nor the existing primary hash chain is
  described as external WORM delivery. Off-host witnessing remains a later
  release gate.
- Each database call, custody call, page, and item has a bounded monotonic
  deadline; each durable phase must make measurable progress. An inventory-derived
  overall ceiling bounds one START/RESUME/finalize request without imposing an
  arbitrary row cap. Deadline exhaustion cleanses the secret and every protected
  workspace and leaves or restores the sealed barrier before returning.

## 1. Dedicated mutation database authority

### 1.1 Extend the dedicated runtime, not the ordinary pool

D2b's current default database adapter leases `db2_pool`. D3a intentionally
revokes the rewrap functions from that runtime role. D3b must therefore provide a
dedicated implementation of the frozen `db2_vault_rewrap_ops_t` over the
non-pooled `db2_vault_operator_runtime_t` connection.

The dedicated adapter owns:

- verified LOGIN session identity, NOINHERIT posture, and explicit capability
  role activation;
- parameterized nonblocking libpq send/flush/consume/result-drain operations;
- explicit SERIALIZABLE transaction handles bound to the owning thread and
  connection mutex;
- the D2a receipt, row, cursor, count, state, and SQLSTATE validation contract;
  and
- connection discard after any role/GUC/protocol/transaction ambiguity, timeout,
  poisoned SQLSTATE, rollback failure, or uncertain COMMIT.

Extract and reuse D2a's strict decoders and SQLSTATE classification where
practical. A second adapter must not accept shapes, lengths, states, or cursor
progress that `org_vault_rewrap.c` rejects. The D2b state machine itself remains
unchanged apart from receiving the dedicated dependency adapter at its production
call site.

Each query receives a monotonic deadline. A transaction may span bounded D2 page
calls and permitted local verification, but a mutex wait or single database
operation cannot block operator shutdown indefinitely. COMMIT uncertainty returns
safe-retry and forces a new private discovery/status snapshot before another
edge.

### 1.2 Private schema functions

Add narrowly scoped `SECURITY DEFINER` functions in
`aimee_kb_vault_orchestrator_api`, each with `search_path=pg_catalog`, fully
qualified objects/operators/casts, exact input bounds, the existing advisory
lock order, and deterministic SQLSTATEs:

- atomic request reservation/replay by canonical peer actor, canonical request
  ID, application-generated operation ID, and exact old/new generations. It uses
  the unique request constraint plus `INSERT ... ON CONFLICT DO NOTHING`, then
  locks and returns the stored actor/request/operation/generation tuple, state,
  epoch, and fence;
- sole-active discovery for RESUME, rejecting duplicate, hidden, terminal, or
  marker-inconsistent histories;
- exact completed material lookup returning only the canonical receipt, receipt
  digest, operation identity, generations, seal epoch, and fence required for
  finalization;
- bounded current KEK-check pages with a strictly advancing UTF-8 byte cursor;
- atomic completed-to-open returning the exact newly inserted or replayed opened
  event identity and row hash; and
- atomic sealed-idle-to-open.

Receipt bytes are available only to the dedicated capability and are never
returned over the operator socket. The current-check page exposes wrapped
verifiers only to the in-process protected callback.

Grant the capability exactly the private discovery/finalization functions and the
D2 mutation closure it needs. It receives no table DML, sequence privilege,
`public` schema USAGE, owner membership, ordinary runtime functions, management
functions, or unrelated vault functions. The LOGIN role remains NOINHERIT and
effectless before the exact `SET ROLE`.

ACL tests enumerate the effective invocable closure by function identity and
schema, not merely explicit grants.

### 1.3 Stable dispatch identities

The START request ID is exactly 16 bytes on the wire and exactly 32 lowercase
hexadecimal characters in PostgreSQL. The client generates or accepts it before
dispatch and must reuse it for retry. The operation ID is a distinct 16-byte
CSPRNG value generated in application code only after request lookup proves
absence. Neither identity uses a PostgreSQL sequence, serial column, timestamp,
PID, counter, or database RNG.

The atomic reservation function stores actor, request ID, operation ID, old
generation, and new generation together. On conflict it locks the unique request
row and requires byte-exact agreement with the immutable client/request metadata:
actor, request ID, old generation, and new generation. A competing handler's
fresh candidate operation ID is not replay metadata: it is cleansed and ignored,
the stored operation ID is returned, and the function proves that ID names the
one operation row with the same actor/request/generations. A metadata or operation
row mismatch raises the dedicated stable replay-mismatch SQLSTATE (`23505`) and
maps to integrity failure without effects. A true operation-ID collision on an
absent request retries with a fresh in-application CSPRNG value inside the same
reservation transaction; a request-row race is resolved by the locked
`ON CONFLICT` row, never by generating a second logical operation.

The exact actor comparison is normative even though v1 has only the canonical
uid-0 actor. D3b deliberately does not add a MAC: it would authenticate no
identity beyond the already authenticated root Unix peer and would invite a false
anti-spoof claim.

## 2. Protected Unix operator protocol

### 2.1 Reserved opcode activation

Activate D3a protocol-v1 opcodes:

- `2 = START`
- `3 = RESUME`
- `4 = UNSEAL`

The STATUS request and its 80-byte successful response remain byte-identical.
Every connection still carries exactly one request and one response and then
closes. The fixed inherited listener, filesystem ownership/mode checks, trusted
parent, server-side root `SO_PEERCRED`, client-side root peer check, and no-network
rule remain unchanged.

Mutation request payloads are canonical:

- START: 16-byte request ID, unsigned big-endian `u32` secret length, then exactly
  1..4096 secret bytes.
- RESUME and UNSEAL: unsigned big-endian `u32` secret length, then exactly the
  secret bytes.

Embedded NUL, zero length, length above 4096, integer overflow, truncation,
reserved-bit changes, and any trailing byte are bad frames with no mutation.
Header and payload lengths must agree exactly.

Define one fixed mutation-response payload containing a closed operation result
and the existing 80-byte secret-free status projection. Operation results are:

- `operational`
- `safe_retry`
- `busy`
- `wrong_secret`
- `backend_unavailable`
- `recovery_required`
- `integrity_failure`
- `invalid_state`
- `unsupported`

Transport/framing/internal results stay distinct from operation results. Unknown
numeric values, invalid result/status combinations, nonzero reserved bytes, or a
successful result without exact `operational` status are protocol integrity
failures.

### 2.2 Secret ownership

After validating only the fixed header and bounded payload length, receive the
secret directly into a page-rounded `mmap` arena protected with `mlock`,
`MADV_DONTDUMP`, and `MADV_WIPEONFORK`. No intermediate heap, stack-sized full
payload, generic string builder, or socket logging buffer may contain it.

The arena is cleansed, munlocked, and unmapped on success, wrong authentication,
disconnect, timeout, parse failure, provider error, database error, shutdown, and
every injected callback failure. Forked children are provider-invalid and see a
wiped arena. Logs and responses may contain only the closed result and secret-free
status; they do not include provider prose, secret length, receipt, TCTI, paths,
or database errors.

Enumerate every function permitted to receive the protected secret: the typed
TPM authorization/PolicyNV probe, D2b START/RESUME provider adapter, guarded
unseal, exact receipt status/cleanup, and their injected test callbacks. Each
callback receives only `(const uint8_t *, size_t)` or a scoped NUL-terminated view
inside the same arena, may not retain it, and is followed by an explicit cleanse
of its scratch region before another callback or outward result. Parse failure,
callback rejection, injected longjmp-equivalent/error return, deadline, client
disconnect, and shutdown all converge on one arena/workspace wipe path. No status,
audit, activation, or response callback runs until the secret and provider scratch
copies have been cleansed.

Client commands are:

```text
aimee-kb vault start --request-id=<32-lowercase-hex> [--json]
aimee-kb vault resume [--json]
aimee-kb vault unseal [--json]
```

The secret is read either from a no-echo `/dev/tty` prompt or an explicit
`--secret-stdin` mode for automation. It is never a positional argument, option
value, environment variable, or JSON field. Text and JSON output use the same
closed result/exit mapping and never echo secret-derived data.

### 2.3 Typed TPM authorization

Add a selected-TPM live authorization preflight before the maintenance guard and
before every START, RESUME, and UNSEAL branch. It authenticates the canonical NV
identity without changing NV or provider state, proves the configured index is
the expected counter with the exact PolicyNV binding and current generation,
proves the daemon still holds its canonical write lock, and returns only:

- authorized;
- wrong secret;
- backend unavailable;
- integrity failure; or
- unsupported.

Every UNSEAL case (`completed_sealed`, `sealed_idle`, and
`local_unseal_required`) additionally performs typed guarded unseal after taking
the guard; a successful preflight never substitutes for authenticating the active
blob. Extend guarded unseal to preserve the same typed distinction. TPM authorization
responses map to `wrong_secret`; TCTI/ESYS reachability and transient device
failure map to `backend_unavailable`; foreign identity, impossible attributes,
generation contradiction, or malformed local context map to integrity failure.
D2b's generic provider failures after an authorized preflight remain sealed
safe-retry/backend failures and never become guessed authentication results.

Only the root-local operator receives this distinction. No remote surface exposes
an authentication oracle.

## 3. Mutation dispatch

### 3.1 Common preconditions

Before START, RESUME, or UNSEAL:

1. authenticate and canonicalize the root Unix peer;
2. fully validate the frame and protected secret arena;
3. revalidate the daemon-lifetime TPM singleton/write lock and canonical NV
   identity;
4. take a fresh complete D3 two-snapshot status;
5. run the live typed TPM authorization/PolicyNV preflight; and
6. only then acquire a maintenance guard or perform any durable effect.

The auth-before-guard order is load-bearing: a wrong secret cannot drain readers,
advance a local epoch, install a barrier, or create a timing-dependent guard side
effect. Tests have a distinct kill/failure point after authorized preflight and
before guard acquisition.

Only one mutation executes at a time. STATUS must remain bounded while mutation
work is active: it may return a typed busy/backend result if it cannot acquire the
dedicated runtime within its existing deadline, but it must not splice an
in-flight transaction. A client disconnect after effects begin does not cancel
the mutation; cancellation stays disabled while a maintenance guard or protected
secret is owned, and the daemon completes or fails closed before cleansing.

### 3.2 START

START always performs the actor/request dispatch lookup before applying a status
gate or generating an operation ID:

- an exact binding routes by the stored operation's authoritative state: active
  enters resume, completed enters finalization, and an already-opened completion
  returns the idempotent operational result after fresh status/singleton proof;
- only an absent request requires exact `operational` status with no relevant
  operation, then reads the live authorized generation, generates a protected
  in-application CSPRNG operation ID, atomically reserves the exact
  actor/request/op/G/G+1 tuple with `INSERT ... ON CONFLICT` replay handling, and
  calls D2b in START mode through the dedicated adapter; and
- replay mismatch, hidden active history, or changed actor metadata is integrity
  failure.

The lookup, status gate, and reservation do not share a database transaction.
Their race is closed explicitly: if an absent lookup is followed by a
non-operational status, the handler performs one locked dispatch re-lookup and
routes an exact binding by its stored state before refusing. If status is
operational, the reservation function returns both the verified row and a
`created` bit. Only `created=true` calls D2b START. `created=false` means another
handler won after the initial lookup; the candidate operation ID is cleansed and
the returned stored row is routed through the same active/completed/opened replay
dispatcher as an initial exact binding. No conflict result can fall through to
START mode, and an exact replay cannot be rejected merely because the winning
handler changed durable status between lookup and gate.

An exact replay compares stored canonical actor/request/generations and proves the
stored operation ID's referential agreement before D2b is called. The independently
generated candidate ID is never compared on the conflict path. Replay mismatch is
the stable SQLSTATE-defined integrity result, not busy or a new operation.

D2b may return completed, safe-retry, busy, aborted, recovery-required, integrity,
invalid, unsupported, or error exactly as today. No D3 wrapper converts an
uncertain edge into success.

### 3.3 RESUME

RESUME is accepted only for `resume_required` or `completed_sealed`. For an active
operation, private discovery must return exactly the operation exposed by the
two-snapshot status. D3b then calls D2b RESUME with that operation and the
peer-derived actor context. It never adopts a caller-selected or historical row.

For `completed_sealed`, RESUME skips D2b mutation and enters exact finalization.
`recovery_required`, integrity failure, backend unavailable, idle sealed, and
operational-without-operation are effectless typed refusals.

START/RESUME continue into finalization when a fresh durable snapshot proves exact
`completed`. A process death between D2b completion and D3b finalization is safe:
restart exposes `completed_sealed`, and RESUME or UNSEAL repeats finalization.

### 3.4 Deadlines, progress, and overall ceiling

Use one monotonic timing budget object shared by dispatch, D2b, and finalization.
It enforces:

- a fixed bounded deadline for each role check, database send/drain, TPM/provider
  call, filesystem cleanup step, mutex/condition wait, page fetch, and individual
  wrap/check item;
- an explicit progress token after each durable phase and strictly advancing page
  cursor/item count within a phase; and
- an overall ceiling derived from the authoritative inventory counts, bounded
  per-item costs, fixed durable edges, and fixed finalization costs, with overflow
  checked before mutation.

The ceiling scales with a legitimate inventory and therefore does not impose an
arbitrary retained-row maximum, but a corrupt count cannot grant an unbounded
runtime. No phase may reset or extend its budget merely by retrying the same
state. If a deadline expires before the durable begin, return backend/safe-retry
without effects. After the barrier may exist, stop at the next fail-closed
boundary, seal, cleanse the secret, KEK/DEK workspace, receipt/pages/cursors, and
return a typed safe-retry or recovery result based only on a fresh durable
snapshot. Cancellation and client disconnect do not bypass this cleanup.

## 4. Terminal cleanup and completed-to-open finalization

### 4.1 Protected finalizer choreography

Finalization owns a new maintenance guard and follows this order:

1. Revalidate the TPM singleton and read exact completed material through the
   private capability.
2. Require sealed durable control, no maintenance identity, one sole outstanding
   completed obligation above `last_opened_rewrap_fence`, and exact operation,
   receipt, digest, epoch, fence, and generation agreement with status.
3. Before acquiring the guard, repeat the bounded live typed authorization,
   canonical NV identity/attributes, PolicyNV generation, and singleton write-lock
   preflight for this finalization attempt.
4. Acquire the guard and synchronize it to the durable sealed epoch.
5. Require typed custody status `INSTALLED` or `CLEANED` for the canonical receipt.
6. If installed, call `vault_custody_tpm2_reseal_cleanup` with
   `VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED`, the typed terminal-only authorization.
   Require a subsequent status of `CLEANED`. Cleanup unlinks only the exact
   continuation bundle and fsyncs its parent. No other enum value or untyped state
   authorizes deletion.
7. Perform typed guard-unseal of the installed active blob; distinguish wrong
   secret, backend failure, and integrity even though preflight already succeeded.
8. Inside `guard_with_active_kek`, constant-time verify the KEK digest bound into
   the receipt, then consume every current KEK-check page. Every nonempty check
   must verify; empty checks remain valid empty rows. Counts, strict cursor
   progress, one terminal empty page, and no hidden extra row are mandatory.
9. Commit the atomic completed-to-open database edge and receive the exact opened
   event ID and row hash created or replayed by that same transaction.
10. Read that exact event by identity through the private authority and verify its
    operation, actor, request correlation, state, fence, epoch, event ID, and row
    hash. Do not require the global audit-chain head to equal this event: concurrent
    legitimate audit appends may have advanced the head after COMMIT.
11. Synchronize the still-exclusive local guard to the returned committed open
   epoch.
12. Revalidate singleton identity, provider-unsealed state, database result, and
    local epoch, then release via the operational guard finish.
13. Cleanse the secret and return a fresh two-snapshot `operational` status.

No pointer to the guard's KEK arena survives its callback. Receipt, page, cursor,
digest, and verifier buffers are cleansed on every exit.

### 4.2 Atomic completed-to-open edge

The SERIALIZABLE private function takes the exclusive control advisory lock and
locks the control and operation rows. It requires:

- exact `completed` operation and immutable completed outbox checkpoint;
- exact receipt/inventory/stage digests and operation fence;
- sealed control, empty maintenance identity, and matching seal epoch;
- `last_opened_rewrap_fence` below the completed operation fence;
- D3a's exact outstanding-obligation rule: an obligation is a `completed` or
  `recovery_required` row above the opened marker; exactly zero or one is allowed,
  and when one exists no greater-fence row of any state may exist. This open edge
  requires that sole obligation to be this exact completed row;
- positive non-exhausted control epoch/fence; and
- canonical peer actor and request correlation metadata.

In one transaction it:

1. appends one deterministic append-only `opened` rewrap event;
2. sets `last_opened_rewrap_fence` to the completed operation fence;
3. sets `sealed=false`;
4. advances control `seal_epoch` and `fencing_token`; and
5. returns the new epoch/fence, exact opened marker, opened event ID, and opened
   event row hash.

Exact replay of the same already-opened completion returns the same logical result
only after proving the marker, exact event identity/hash, control, and operation
agree. The caller reads and verifies that exact event after COMMIT. It never pins
or compares the mutable chain head, because an unrelated legitimate concurrent
append may already be its successor. Changed replay is integrity failure. A later
operation must not be hidden by replaying an older open.

If COMMIT is uncertain after local unseal, D3b seals locally, discards the
connection, and resnapshots. A committed open then appears as
`local_unseal_required`; an uncommitted edge remains `completed_sealed`. Cleanup
does not need restoration in either case because `completed + CLEANED` is a valid
resumable state.

### 4.3 Operational guard finish

Add a separate `vault_maintenance_guard_end_operational` rather than weakening
the existing fail-closed `vault_maintenance_guard_end`.

The operational finish succeeds only when:

- the caller owns the exact live guard and no callback is active;
- the selected provider reports unsealed and can supply its KEK;
- the primary epoch has been initialized to the exact committed open epoch;
- the local use epoch can advance without overflow; and
- no fork invalidation or singleton failure has occurred.

It cleanses/unmaps the maintenance arena, clears the registry, restores
cancellation, and releases the exclusive use lock without calling provider seal.
Every failed precondition takes the existing sealing end path. The ordinary guard
end remains seal-on-exit and all D2b callers retain that behavior.

## 5. Explicit UNSEAL cases

UNSEAL accepts only these exact status shapes:

### `completed_sealed`

Run the complete finalizer above, including the live typed PolicyNV/NV
identity/write-lock preflight before the guard and typed active-blob unseal after
it. The operation is not opened merely because its TPM artifact is installed or
cleaned.

### `sealed_idle`

Repeat the live typed PolicyNV/NV identity/write-lock preflight, acquire the
maintenance guard, sync the current sealed epoch, typed-unseal the active blob,
verify its KEK against every current nonempty database check, then commit an
analogous sealed-idle-to-open function. That function requires no
relevant operation, sealed idle control, an unchanged marker/history, and advances
the epoch/fence while appending the deterministic primary audit/outbox record.
After commit, sync the guard and finish operational.

### `local_unseal_required`

Durable control is already open. Repeat the live typed PolicyNV/NV
identity/write-lock preflight, acquire the guard, synchronize its exact current
epoch, typed-unseal with the closed wrong-secret/backend/integrity result, verify
the active KEK and all current checks, revalidate the unchanged open database
tuple and singleton, then finish operational. It creates no fabricated completed
transition and does not advance the opened marker. A
deterministic local-unseal audit event may be appended through the private
authority, but audit failure must seal and refuse activation.

All other states refuse without opening. In particular, UNSEAL cannot bypass an
active operation, quarantine, malformed marker, unavailable backend, or integrity
failure.

## 6. Runtime activation

D3a's non-operational startup currently enters a STATUS-only wait and exits without
initializing general surfaces. Restart cannot be the activation mechanism because
the required D3 startup path force-seals the provider again.

Replace that terminal wait with a one-way mutex-plus-condition-variable activation
latch owned by `kb_main`. The mutation thread publishes the complete committed
epoch/fence/event result under the latch mutex, then signals; the main thread waits
in a predicate loop and consumes it only after the condition wait's release/acquire
happens-before edge:

1. Before activation, only the fixed STATUS/mutation UDS service exists.
2. A successful operational guard finish publishes the latch only after a fresh
   two-snapshot operational status and singleton revalidation, within a fixed
   bounded publish window measured from open COMMIT. Window expiry seals and does
   not publish.
3. `kb_main` wakes through the mutex/condition release/acquire edge, independently
   repeats exact operational status and singleton checks under the remaining
   bounded publish window, compares the committed epoch/fence/event identity, and
   continues the existing HTTP, mTLS, egress-recovery, and worker initialization
   exactly once.
4. A failed mutation leaves the process in local operator mode and the latch
   unpublished.

The operator service remains available after general serving begins. A later
START installs the durable primary barrier and takes the local exclusive guard;
already-admitted uses drain, new key-use admissions fail closed, and nonsecret
surfaces may remain up. Only exact finalization reopens egress. A transition to
`recovery_required` never publishes activation.

Shutdown closes mutation admission, wakes accepted connections, waits for the
non-cancellable guarded mutation to reach a fail-closed boundary, joins the
operator service, closes the dedicated connection, shuts down general workers and
DB2, and releases the TPM singleton last.

## 7. Failure/result rules

- Wrong secret makes no database or filesystem mutation and leaves the provider
  sealed when it began sealed.
- Busy singleton/guard/mutation ownership is retryable and does not create a
  terminal operation.
- Backend unavailability is distinct from wrong secret and integrity; it never
  opens durable control.
- Database transient/serialization/COMMIT uncertainty is safe-retry after a fresh
  status. Error prose is neither parsed nor returned.
- Receipt, generation, artifact, marker, history, KEK digest, current verifier,
  or role/GUC mismatch is integrity failure and stays sealed.
- An exact D2b `recovery_required` result remains durable quarantine and does not
  attempt cleanup or unseal.
- Client timeout/disconnect cannot cause a partially completed mutation to be
  reported as failure while it continues invisibly. The daemon completes the
  guarded edge, records its closed result internally, and the client resolves it
  through stable request/status replay.
- No response claims `operational` until the open transaction is durably known,
  local epoch synchronization succeeds, the guard finishes operationally, and a
  fresh two-snapshot status agrees.

## 8. Validation and acceptance gates

### Unit and injected state machines

- Every operator status x opcode combination, including all effectless refusals.
- START absent/exact replay/changed actor/request/op/generation replay, stable
  mismatch SQLSTATE, atomic `ON CONFLICT` request race, in-app CSPRNG collision,
  and proof that no sequence/database RNG supplies either identity.
- RESUME sole-active discovery, hidden/duplicate/history corruption, and exact
  completed handoff.
- Dedicated adapter parity with every D2a decoder rule, transaction phase, cursor,
  page count, SQLSTATE, timeout, rollback failure, and uncertain COMMIT.
- Typed TPM auth/PolicyNV preflight and typed unseal on START, RESUME, and all
  three UNSEAL branches: correct secret, wrong secret, unavailable backend,
  foreign NV identity/attributes, generation drift, lost write lock, malformed
  provider, and non-TPM unsupported.
- Finalizer at installed and cleaned status; cleanup/unseal/open/epoch/finish
  failure at every call boundary.
- Completed-open and idle-open exact replay, fence/epoch exhaustion, marker
  regression, D3a outstanding-obligation violations, later-row hiding, duplicate
  event, exact event-ID/hash verification with a concurrently advanced legitimate
  chain head, and changed actor/correlation.
- Operational guard finish with wrong owner, active callback, sealed provider,
  stale epoch, overflow, fork invalidation, singleton loss, and cleanup failure.
- Protected-arena canaries and cleanse assertions for every request/parser/provider
  exit and every enumerated auth, orchestrator, status, cleanup, unseal, and test
  callback failure.
- Per-database/TPM/filesystem/page/item deadlines, nonprogress, count overflow,
  inventory-derived overall ceiling, client disconnect, and deadline-triggered
  workspace cleansing/sealed recovery.
- Activation latch predicate-loop, mutex/condition release-acquire visibility,
  spurious wakeup, duplicate signal, stale epoch/event payload, publish-window
  expiry, and main-thread bounded status/singleton recheck.

### Unix protocol and fuzzing

- Wrong peer uid, wrong inherited fd/path/mode/owner/parent, symlink and abstract
  socket.
- Bad magic/version/opcode/reserved, truncated header/payload, zero/oversize
  secret, embedded NUL, integer overflow, immediately readable surplus, slowloris,
  disconnect before and after the first effect, and response backpressure.
- STATUS remains wire-compatible. Mutation results reject unknown enums,
  malformed status pairings, nonzero reserved bytes, and surplus payload.
- Bounded deterministic frame fuzz plus ASAN/UBSAN/LSAN; no secret or receipt is
  printed by text/JSON clients.

### PostgreSQL 17

- Upgrade and clean schema installs; schema-sync and the full P1 RLS gate.
- Exact effective capability closure after LOGIN and `SET ROLE`.
- LOGIN-before-role and `aimee_kb_runtime` are denied every mutation, receipt,
  check-page, open function, rewrap table, and sequence.
- Capability is denied all unrelated public/private application functions and
  direct DML.
- Concurrent status/dispatch/open versus every D2b transition; request and actor
  collision; SERIALIZABLE retry; connection loss at BEGIN/query/COMMIT/result
  drain.
- `opened` event, marker, control open, epoch, and fence are atomic; trigger-backed
  append-only rules reject update/delete/truncate.
- Current-check paging covers empty/nonempty checks, more than two pages, UTF-8
  byte ordering, duplicate/regressing cursor, hidden extra rows, and tampering.

### CT260 real-daemon kill matrix

Build exact head with PostgreSQL 17, libtss2, swtpm, and the integration-only
loopback override. Drive an actual aimee-kb process only through the fixed
root-owned UDS/CLI and rotate more than 257 retained DEKs plus multiple empty and
nonempty checks.

Kill the daemon after each externally observable or durable boundary:

1. frame parse and typed authorization;
2. authorized PolicyNV/NV/write-lock preflight before guard acquisition;
3. request dispatch and begin COMMIT;
4. prepared bundle visibility;
5. receipt record;
6. staging COMMIT;
7. committing intent;
8. NV increment;
9. active-blob rename and parent fsync;
10. resealed checkpoint;
11. database promotion;
12. verification/completion COMMIT;
13. typed terminal-only cleanup authorization, unlink, and parent fsync;
14. active-blob typed unseal and KEK/check verification;
15. open COMMIT and exact opened-event identity/hash verification while another
    legitimate audit event advances the chain head;
16. local epoch synchronization;
17. activation-latch publication and main-thread bounded recheck; and
18. mutation response.

After each kill, restart using only PostgreSQL, swtpm state, and the artifact
directory. Status must be exactly one safe state: `resume_required`,
`completed_sealed`, `local_unseal_required`, `operational`, or durable
`recovery_required` according to the boundary. Replaying the same request must
never create a second operation, NV increment, completed/opened event, marker
advance, or open edge. Old active blobs remain denied by PolicyNV.

Also prove:

- wrong secret for every opcode causes zero durable/open progress;
- a second daemon for the same NV cannot reach mutation or general serving;
- singleton revalidation failure before final open stays sealed;
- corrupt/missing bundle components, external NV advance, old-blob replay,
  receipt/check tampering, PG disconnect, and response loss all classify without
  secret leakage;
- restart never auto-resumes and never persists the authorization secret; and
- database rows, artifacts, logs, stdout/stderr, crash files, and core-dump
  candidates contain no raw secret, KEK, or DEK canaries.

### Repository gates

- Default non-TPM build returns effectless `unsupported` and contains no accidental
  mutation grant or provider fallback.
- Production TPM2 build proves the loopback override symbol and path are absent.
- `make kb`, server build, lint, schema sync, focused and full unit tests,
  ASAN/UBSAN/LSAN, D1 prepared-reseal gates, D2a PG17 gates, D2b fake/real matrices,
  D3a status/singleton/UDS gates, and build-integrity checks remain green.
- Adversarial branch roundtable receives the full plan and full staged diff,
  findings are individually validated, and every genuine finding is fixed before
  merge.

## Explicit exclusions

- External/off-host WORM delivery, acknowledgement, backlog enforcement, signed
  witness heads, and sink failover.
- Clearing or repairing `recovery_required`, recovery quorum, break-glass,
  destructive re-provision, and old-generation reactivation.
- Automatic resume, automatic unseal, persisted TPM authorization, scheduling,
  UI, or remote operator API.
- File-custody to TPM migration or a generic `protection_enabled` conversion.
- Remote TCP/mssim/swtpm production TCTIs.
- Multi-runtime or distributed TPM reseal, distributed reader draining, and fleet
  cache convergence.
- KMS or PKCS#11 root activation/rotation receipts and SoftHSM/KMS production
  enablement.
- External egress broker isolation or changes to the existing D2b cryptographic
  state machine beyond selecting its dedicated production adapter.

## Completion criteria

D3b is complete only when a root-local operator can start or resume the exact
whole-vault operation, crash at every durable boundary, restart sealed, and reach
one idempotent operational state through the same fixed UDS without exposing a
secret or granting the ordinary runtime mutation authority. Durable open,
`last_opened_rewrap_fence`, the deterministic opened event, local provider state,
and the synchronized use epoch must agree before any org-key egress surface is
activated. Any uncertainty or contradiction remains sealed and observable rather
than guessed or repaired.
