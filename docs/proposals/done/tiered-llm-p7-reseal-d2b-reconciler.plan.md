# P7-reseal-d2b in-process TPM2/Postgres reconciler

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered and validated on PostgreSQL 17 plus swtpm (CT260), with
  default and ASAN/UBSAN builds; production-uninvoked until D3 operator
  enablement.
- **Depends on:** P7-reseal-a/c/d1 and P7-reseal-d2a.
- **Enables:** D3 operator authorization, external WORM delivery, cleanup,
  operational unseal, and the exhaustive restart/kill matrix.

## Scope and enablement boundary

Add the bounded, synchronous state machine that reconciles the owner-only
Postgres whole-vault rotation record with the canonical TPM2 prepared-reseal
protocol. It performs start or exact-operation resume while holding the existing
process-local exclusive maintenance guard, stages every retained DEK and KEK
check under a protected fresh KEK, commits custody, promotes the staged database
material, and cryptographically verifies the promoted inventory before durable
completion.

The reconciler is linked into the kb build and has real default adapters, but no
route, RPC, CLI, scheduler, startup worker, signal hook, configuration switch, or
automatic caller. It does not grant runtime database authority, clean prepared
artifacts, drain or acknowledge the WORM outbox, or leave the provider unsealed.
Those activation and operational-safety decisions remain D3. The non-TPM build
returns `UNSUPPORTED` through an explicit adapter capability check, without
beginning maintenance or mutating Postgres.

## Public and injected seams

Add `modules/vault/vault_reseal_orchestrator.[ch]` with a closed request/result
contract. A request supplies an exact 16-byte operation ID, an actor of 1..575
octets, a request ID of 1..200 octets, and an opaque provider secret with an
explicit conservative maximum length. Embedded NULs and overlong secrets are
rejected before effects. Callers choose
`START` or `RESUME`; start obtains the authoritative TPM generation, requires
`G < INT64_MAX`, and creates the database intent exactly once. Resume requires
the named durable operation and never adopts another active operation. This
slice treats the operation ID as the recovery capability; active-operation and
request-ID discovery remain part of D3's authorized dispatch surface.

Results distinguish completed/exact terminal replay, safe retry after a
transient or uncertain database result, busy, pre-increment abort, sealed
recovery-required, integrity failure, invalid input, unsupported provider/build,
and generic failure. Output is initialized before validation and contains only
state/generation/fence metadata, never key material or database payloads.

Freeze two const vtables:

1. the already-complete `db2_vault_rewrap_ops_t`; and
2. a custody/guard adapter containing NV-generation, prepare, discover,
   recover-KEK, status, commit, guard begin/sync/unseal/seal/with-active-KEK/end,
   an explicit supported-build check, and cryptographic random/wrap/unwrap/check
   operations.

The default custody adapter calls only the existing typed public seams. Tests
inject total fakes and can fail every call boundary. Vtables are validated before
any effect; no production global is rebound for tests. Database operations are
reachable only through the D2a vtable: the reconciler contains no SQL, PG adapter
call, table name, or SQLSTATE/message parsing.

## Secret ownership and guard choreography

The entire call is synchronous and owns one maintenance guard from before the
first durable read/write through final fail-closed sealing. Cancellation remains
disabled by the guard. The provider is explicitly unsealed under the guard when
the current phase needs its active KEK; ordinary readers remain excluded.

Copy the bounded operator secret into its own page-rounded protected arena and
NUL-terminate it only for the current TPM API. Allocate a second protected
workspace holding the fresh/recovered KEK and one plaintext-DEK scratch slot in
a page-rounded `mmap` arena with
`mlock`, `MADV_DONTDUMP`, and `MADV_WIPEONFORK`. Failure to obtain every
protection fails before TPM prepare. Generate the key directly into that arena,
or recover directly into it. Cleanse, `munlock`, and `munmap` it on every exit.
No heap copy, raw-key getter, log, result, durable row, or callback-surviving
pointer is permitted for either secret. Fork invalidation from the guard is
inherited; a child cannot continue reconciliation.

Old-KEK staging and new-KEK verification occur only inside
`guard_with_active_kek` callbacks. The staging callback receives the protected
new-KEK workspace as context, opens one SERIALIZABLE staging transaction, validates
each nonempty old principal check, unwraps each 40-byte source DEK, immediately
rewraps it under the new KEK, stages it, and cleanses the plaintext DEK and page
buffers. Empty checks stay empty; nonempty checks are verified under the old KEK
and regenerated under the new KEK. It consumes bounded pages until an
authoritative empty page and commits only after `stage_finish` succeeds.

After custody is installed and Postgres is promoted, the guard seals the old
cache, unseals the installed provider artifact, and a second active-KEK callback
opens one SERIALIZABLE verification/completion transaction. It checks the
summary, unwraps every promoted DEK, verifies every nonempty KEK check, consumes
exactly the summary counts plus authoritative empty pages, records the typed
crypto acknowledgement, completes with the exact three digests, and commits.
Empty checks count as verified rows. Plaintext DEKs, page rows, cursors, receipt
bytes, digests, and all arena storage are cleansed on all exits.

No database transaction spans an RNG, TPM, provider, guard, or custody call.
The only crypto inside a database transaction is local AES-KW under keys already
present in protected memory, as allowed by D2a.

## Reconciliation state machine

Every mutating database edge is one explicit transaction. After the durable
begin barrier, synchronize the guard to the committed seal epoch before any KEK
access. A successful commit is followed by a fresh snapshot before another edge. A transient error or commit
uncertainty returns `SAFE_RETRY`; it never guesses whether the edge committed.
Exact replay states returned by D2a are accepted only after a fresh snapshot
rebinds the operation, fence, generations, receipt, and digests.

For every nonterminal snapshot, discover the canonical operation bundle and bind
any returned receipt byte-for-byte to the durable canonical receipt. Conflict,
corruption, generation drift, receipt mismatch, or an impossible state/status
pair becomes `recovery_required` when that transition can be durably recorded;
if quarantine itself is uncertain, return sealed `SAFE_RETRY`. Unknown enum
values are integrity failures and never drive an effect.

The allowed forward table is:

| Postgres state | TPM status | action |
|---|---|---|
| `preparing` | absent at G | generate protected KEK, prepare, record receipt |
| `preparing` | exact prepared at G | recover KEK, record receipt |
| `custody_prepared` | exact prepared at G | recover KEK, stage all wraps/checks |
| `wraps_staged` | exact prepared at G | record committing, then resnapshot before custody commit |
| `reseal_committing` | prepared/NV-advanced/installed | idempotent custody commit/repair; require installed |
| `resealed` | exact installed | promote |
| `promoted` | exact installed | unseal, verify all promoted material, complete |
| `completed` | exact installed or cleaned | return completed, still seal locally; never cleanup here |

All unlisted active-state/status combinations, including `CLEANED`, conflict,
corrupt, and custody-ahead states, quarantine. `aborted` and
`recovery_required` are terminal inspection results. Completed accepts only an
exact installed artifact or already-cleaned continuation state; all other
terminal combinations are sealed integrity errors.

`wraps_staged` paired with NV-advanced/installed is quarantined: a conforming
driver cannot advance custody before a freshly confirmed `reseal_committing`
intent, so reconstructing that intent would bless an unaccounted irreversible
act. `reseal_committing` at prepared calls commit; at NV-advanced/installed it
also calls idempotent commit to verify/repair the active artifact. After exact
installed custody, record the canonical receipt digest as `resealed` before
promotion. `CLEANED` before database completion is always quarantined because
continuation evidence was removed without terminal authorization. `resealed`
and `promoted` accept only exact installed custody.

Before NV advances, absent/lost prepared material or a conclusively failed
prepare with no exact durable artifact may call custody abort when a receipt
exists and then the typed database abort edge. Old-KEK/check/DEK or inventory
integrity failure is quarantined instead of reopening a known-corrupt vault.
Abort is allowed only from
`preparing`/`custody_prepared`/`wraps_staged`, only after status proves NV remains
G, and never after a commit attempt. Custody status and NV generation are
rechecked after artifact abort and before database abort; this ordering is a
mandatory crash test because `aborted` has no later quarantine edge. A failed or uncertain custody abort retains
the durable sealed barrier and returns recovery-required rather than clearing it.

Every failed or uncertain `prepare` is followed by canonical discovery before
any retry, abort, or terminal database edge. A durably-created artifact whose
prepare response was lost is therefore adopted only through its exact discovered
receipt and is never overwritten or mistaken for absence.

Early database states (`preparing` or `custody_prepared`) paired with
NV-advanced/installed cannot recreate the lost new KEK and are quarantined.
`resealed`/`promoted` require installed custody. `aborted` and
`recovery_required` are terminal typed results; they never invoke custody or
clear barriers. An already-aborted operation with unexpectedly advanced custody,
or a completed operation with mismatched custody, is a sealed fatal/report-only
condition because neither terminal state has an outgoing edge. D2b never invokes
prepared-artifact cleanup.

The driver has a fixed maximum number of durable edges per call and checks signed
count/cursor overflow. Large inventories are bounded per page but not
artificially capped: page progress is bounded by the snapshot's exact row counts, with
one required terminal empty page. Any excess, duplicate, regression, missing
row, nontermination, or count disagreement is integrity failure.

## Failure and sealing rules

Map failures to stable lowercase classes such as `custody_integrity`,
`generation_conflict`, `receipt_mismatch`, `source_integrity`,
`verification_integrity`, and `provider_failure`; never persist vendor/database
error prose. BUSY and transient database outcomes do not become terminal rows.
Invalid caller input has no effects. Custody `BUSY` and database transient or
commit-uncertain outcomes are safe retry. The current custody generic error
conflates authentication, I/O, and provider failure, so it remains sealed and
retryable rather than becoming a terminal row. `NOT_BUILT` is unsupported.

Once the durable maintenance barrier may exist, every return path attempts
guard seal and guard end, and reports failure if either cannot fail closed.
Successful completion deliberately remains sealed. No result claims completion
until the completion transaction's commit is known successful or a fresh
snapshot proves exact `completed` state. Recovery-required never attempts
operational unseal after quarantine.

This protocol inherits the umbrella's pinned single-active-runtime deployment
limit. The process-local guard plus primary admission barrier closes all races in
that topology, but cannot drain a reader already admitted in a second process.
D3 must prove the single-runtime deployment precondition before exposing any
caller; multi-runtime/provider rotation requires a separate distributed
activation protocol and is not enabled by this callable seam.

## Validation gates

- Exhaustive fake DB/custody state-machine tests cover every database state × TPM
  status, exact resume, every injected call failure, commit uncertainty, receipt
  mismatch, generation drift, abort eligibility, quarantine, idempotent commit,
  terminal replay, bounded-page termination, hidden extra rows, cursor/count
  attacks, and proof that no transaction spans a custody/guard call.
- Crypto tests rotate multiple secrets and checks including empty checks; wrong
  old/new KEKs and every-byte wrap tampering fail closed. Canary assertions prove
  DEKs, receipts, page buffers, cursors, the protected secret arena, and every
  protected workspace slot are cleansed before unmap.
- Guard tests cover initially sealed/unsealed providers, callback failure,
  seal/unseal/end failure, cancellation state, concurrent readers, same-process
  competing reconciliation, and fork invalidation without timing guesses.
- Default and TPM2 builds, focused ASAN/UBSAN, and fuzzed state/status/request
  decoding pass. The default non-TPM adapter returns unsupported and performs no
  DB operation.
- CT260 runs a real PostgreSQL 17 + swtpm happy path with more than two pages,
  process restart at each already-supported durable boundary, idempotent resume,
  old-blob replay denial, wrong secret, corrupt/missing prepared components, and
  raw-key canary scans. The full signal-level kill matrix, external WORM drain,
  cleanup, and production entrypoint remain the D3 release gate.
- Source/link inventory proves there is still no route, CLI, scheduler, startup
  worker, runtime grant, cleanup call, or automatic production invocation.

## Completion criteria

D2b completes when the callable default reconciler can take one exact operation
from start or any recoverable durable state through sealed `completed`, every
state/status mismatch fails closed through the typed abort/quarantine policy,
all secret-bearing work stays inside protected/cleansed memory and the exclusive
guard, real PG17+swtpm validates the integrated protocol on CT260, and no
production mechanism can invoke it before D3.

## Delivered validation

- The default fake gate covers all nine database states against all seven TPM
  statuses, a 257-secret/257-check three-page forward path, both authoritative
  terminal empty pages, START replay, custody-ahead prepare uncertainty, abort
  reclassification ordering, retained-transaction rollback, callback and guard
  failures, commit uncertainty, and forbidden call traces. Focused ASAN/UBSAN
  with leak detection passes.
- Default server and kb builds pass. The orchestrator object is linked only into
  the kb vault closure, and source inventory finds no route, CLI, scheduler,
  startup worker, cleanup call, or other production invocation.
- CT260 passed the on-demand real PostgreSQL 17 + libtss2 + swtpm gate: wrong
  secret with no database edge; full START over 257 DEKs and five checks
  including an empty check; sealed completion; old-KEK rejection and installed
  KEK verification; old-blob PolicyNV replay denial; fresh-process resume from
  `preparing` and `custody_prepared`; corrupt-bundle durable quarantine; retained
  continuation artifacts; and raw old/new key canary scans across PostgreSQL,
  artifacts, and logs.
- Adversarial implementation reviews found and closed missing D2a terminal-empty
  page consumption, post-abort custody reclassification, stale terminal output,
  receipt/evidence quarantine gaps, uncertain-prepare KEK adoption, retained
  transaction cleanup, aborted-terminal custody inspection, START replay
  mismatch classification, and custody-ahead failed-prepare handling. The final
  sealed review additionally hardened scratch-database identifier handling,
  fail-closed non-Linux compilation, injected page-count/cursor validation, and
  typed epoch-sync results; the hardened CT260 gate remained green.
