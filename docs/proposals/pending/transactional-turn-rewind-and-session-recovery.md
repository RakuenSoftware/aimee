# Turn-scoped change sets and safe workspace restore

- **State:** roundtable-approved — design candidate, revision 4.
- **Scope:** deterministic execution safety for Aimee-managed worktrees.
- **Charter fit:** strengthens existing **Execute / Persist / Enforce / Review**
  boundaries. It adds no intelligence role, policy engine, conversation store, workspace
  abstraction, or approval system.
- **Implementation posture:** concepts and integration contracts only. An implementation
  plan must pin final types, migrations, and signatures before code work begins.

## Executive decision

Extend the existing `fsnap` / `aimee rewind` feature into a server-owned,
turn-scoped change-set service. The safe unit is one completed Aimee execution in one
managed worktree, not a git branch, a model tool round, or a conversation branch.

The first release is deliberately narrow:

- co-located `WS_PROVIDER_SHARED` workspaces only;
- Aimee-managed session worktrees only;
- native `write_file` and `edit_file` mutations only, including file creation;
- regular-file and absent-file preimages only;
- conflict-free restore only; no force override;
- filesystem restore only; no conversation truncation or history rewriting;
- detected-sensitive content is refused, not encrypted;
- arbitrary shell/git turns are observe-only and never advertised as safely restorable;
- apply restores the complete sealed set; partial/path-selected restore is deferred.

This yields a useful feature without claiming guarantees the current workspace-resource,
governance, or conversation layers do not provide.

## 0. Why Git `HEAD` is not the turn baseline

Git remains the repository-history authority and should be used when it is sufficient.
It is not generally the state that existed before an Aimee turn:

```text
HEAD = A
user's pre-existing uncommitted state = B
Aimee's post-turn state = C
```

Undoing Aimee must restore `B`; `git restore --source=HEAD` would restore `A` and destroy
the user's work. The same distinction applies to staged edits, untracked files, and work
created by another actor before the turn.

A Git object is therefore an allowed storage fast path only when the sealed change set
proves that the path matched that exact object at turn start. The change-set manifest,
not `HEAD`, remains the authority for whether the optimization is valid.

## 1. Current Aimee state and concrete gaps

The shipped path is small and useful:

- `src/server/agent_tools.c::agent_tools_begin_turn()` resets a thread-local snapshot id
  for each internal model/tool round.
- `src/modules/tools/agent_tools_dispatch.c` calls `auto_snapshot_record()` before the anchored
  edit, legacy edit, and write-file paths.
- `src/modules/tools/agent_tools.c::auto_snapshot_record()` creates one DB1 snapshot for that
  internal round when `rewind.auto_snapshot` is enabled.
- `src/db1/fsnap.c` stores absolute paths and inline bytes in `file_snapshots` /
  `file_snapshot_entries`.
- `src/cmd_rewind.c` reads and mutates local DB1 directly.
- `webchat/chat.go::handleChatRewind()` is explicitly a branch-management stub.

The current behavior has seven correctness or ownership gaps:

1. `db1_fsnap_record_file()` deletes and reinserts an existing entry. Two writes in one
   snapshot preserve the intermediate state, not the first preimage. The existing
   `test_record_overwrite_within_snapshot` test codifies this unsafe latest-wins behavior.
2. Capture errors are ignored and the file mutation proceeds. The resulting checkpoint
   does not record that coverage is incomplete.
3. Snapshot grouping uses the internal model/tool-loop integer. A single server-owned
   user turn may span several such rounds, so one UI turn can produce multiple unrelated
   checkpoints and lose first-preimage semantics across them.
4. Capture reads the server filesystem directly even when file mutation routes through
   `workspace_provider_active()`. A detached or container-bound turn can therefore
   snapshot the wrong filesystem.
5. Restore follows stored absolute paths, has no owner/worktree check, no no-follow
   resolver, no current-state compare-and-swap, no whole-set journal, and can report a
   partially applied restore only as “with errors.”
6. `aimee rewind` is a stateful direct-DB CLI, so a thin client can inspect a different
   DB1 than the server that owns its session.
7. `--no-restore` deletes DB1 context windows and labels that action “conversation
   truncated,” but server-owned completed history is canonical and webchat thread
   branching is explicitly separate. The command does not rewind a conversation.

Worktree isolation reduces the blast radius. It does not preserve user edits that existed
before a turn or make a multi-file mistake easy to inspect and reverse.

## 2. Existing owners: reuse and boundaries

| Concern | Existing Aimee owner | Integration decision |
| --- | --- | --- |
| User-turn identity and cancellation | `server_compute_async.c`, presence turn id, `turn_registry`, and [server-owned turn lifecycle](../done/server-owned-turn-lifecycle.md) | One primary change set is keyed by the server-owned presence turn id. Internal tool rounds become evidence only. |
| Session ownership | `server_sessions.principal`, attested request identity | Every list/preview/restore request re-authorizes against the session row. A set id is never authority. |
| Worktree mapping | `session_state_worktrees`, `session_isolation_target()`, `worktree_for_cwd()` | Store the session worktree mapping reference plus a stable root identity; do not create another workspace registry. |
| Resource I/O | `workspace_provider_active()` and `workspace_turn_*` | P0–P5 support only shared, managed worktrees. Other provider kinds return a typed unsupported capability and cannot create a restorable set. |
| File mutation guards | read-only delegate, parent-worktree, session-isolation guards in `agent_tools_dispatch.c` | Capture runs after these guards pass and before the provider write. It grants no new authority. |
| Stale-edit prevention | immutable `anchor_snapshot`, `edit_anchored_plan()`, `diff.c`, and [hashline editing](../done/proposal-hashline-edit-and-lean-websearch.md) | Anchored edits pass their already-read current bytes into capture; no second read or parallel snapshot model. `diff.c` renders previews. |
| Tool capability policy | tool registry/guardrails and the capability-scoped execution design | Restore is a governed native operation below capability admission. Approval can never grant workspace or session ownership. |
| Audit | `audit_action_log`, [shipped WORM append/checkpoint](../done/auditable-worm-audit-store.md), and [pending attestable enforcement](governance-attestable-enforcement.md) | Emit bounded change-set events through the common audit bridge; do not add a third audit store. |
| CLI and `/v1` parity | `cli_v1_routes*`, `g_v1_routes[]`, route drift scripts, and [route descriptor convergence](route-descriptor-single-source-of-truth-residual.md) | Server owns behavior. CLI, MCP, and webchat are adapters over the same domain service. |
| Configuration | existing nested `rewind` section and [config descriptor convergence](config-field-descriptor-save-residual.md) | Evolve the existing section. The pending flat-field descriptor proposal does not own nested objects. |
| Health and repair | `cmd_doctor.c`, DB1 diagnostics | Overall `aimee doctor` reports rewind health; `aimee rewind doctor` is only a focused view/action surface. |
| Operator activity | audit endpoints and [operator audit activity surface](operator-audit-activity-residual.md) | The activity UI consumes the same safe audit events when that proposal lands. |
| Appliance recovery | [appliance state recovery runbook (PR #2329)](https://github.com/RakuenSoftware/aimee/pull/2329) | The runbook may call rewind verification; it does not own change-set repair semantics. |

### 2.1 Explicit non-ownership

This proposal does not:

- branch, delete, or rewrite provider conversation history;
- alter the presence ring or make it durable;
- invent per-agent principals or `on_behalf_of`; the pending identity proposal owns them;
- make the WORM store authoritative; the attestable-enforcement proposal owns that
  rollout;
- add a generic transaction layer to `workspace_provider_t` in the first release;
- infer arbitrary shell side effects from command text;
- replace git commits, branches, worktrees, or workflow artifacts;
- reverse network, forge, package, database, remote-host, or process side effects.

## 3. Unit of work and lifecycle

### 3.1 Execution identity

For an interactive primary turn, the authoritative execution key is:

```text
(session_id, presence_turn_id, managed_worktree_ref)
```

`presence_turn_id` is already minted before dispatch in
`chat_stream_async_worker()`. The worker binds that id, the attested principal, and the
resolved worktree to a thread-local mutation context beside the existing request cancel
and vault-principal context. The async wrapper retains the opaque change-set handle in a
local before `chat_stream_worker()` runs, because that worker frees `cctx`; it seals with
the retained handle, then clears the thread-local on every return path.

`agent_tools_begin_turn(int tool_round)` no longer resets the change-set id. It records
only the current internal tool round. Thus writes in tool rounds 1, 2, and 3 of one
server-owned turn share the same first preimage.

Non-presence executions require an explicit stable execution id from their existing
owner before they may be protected:

- delegate: delegation id plus its own worktree;
- workflow: lifecycle work-item/block-attempt id plus its own worktree;
- direct local CLI execution: a server-minted run id.

Until those call sites are wired and tested, they are observe-only. A parent's change set
never absorbs a delegate's separate-worktree changes.

### 3.2 State machine

```text
                 ┌──────────────→ INVALID
                 │
OPEN ────────────┼──→ ABORTED
  │              │
  ├── crash ─→ RECOVERABLE_OPEN ─→ SEALED_INCOMPLETE
  │
  └────────────→ SEALED ─→ RESTORE_IN_PROGRESS ─→ RESTORED_AUDIT_PENDING ─→ RESTORED
                       │             │
                       │             └──────────→ RESTORE_INCIDENT
                       └────────────────────────→ EXPIRED
```

- `OPEN`: accepts immutable preimages and append-only operation evidence.
- `ABORTED`: no completed native mutation was observed. Terminal and not restorable;
  blob refs are released.
- `INVALID`: capture, coverage, identity, evidence, or integrity failure. Terminal and
  never shown as safe.
- `RECOVERABLE_OPEN`: startup/doctor classification only; no automatic restore.
- `SEALED_INCOMPLETE`: an interrupted turn with fully verifiable mutated entries. It may
  be previewed but restore remains an explicit operator action.
- `SEALED`: immutable manifest and final postimages exist.
- `RESTORE_IN_PROGRESS`: durable restore journal written before the first filesystem
  mutation.
- `RESTORED_AUDIT_PENDING`: filesystem and DB1 operational state committed, but the
  separate audit sink has not yet acknowledged the terminal event. Doctor retries the
  bounded event; this state is never reported as fully complete.
- `RESTORED`: requested entries and audit outcome committed.
- `RESTORE_INCIDENT`: apply or compensation could not converge automatically. The
  worktree is write-frozen at `mutation_begin` until operator acknowledgement.
- `EXPIRED`: manifest retained; one or more required blobs were policy-pruned.

No state transition makes a sealed manifest mutable. A later restore attempt receives a
new restore-run id and journal.

Each successful `mutation_prepare` increments a durable prepared-token count in the same
DB1 transaction as its token/entry evidence; `mutation_finish` closes that token and
increments the terminal-token count exactly once. Normal seal requires equality. Startup
classifies an open set as recoverable only when every token row is terminal, every blob
and digest validates, and the two counters agree; otherwise it is `INVALID`, not guessed
complete.

### 3.3 Entry lifecycle and first-preimage invariant

Each path moves through:

```text
CAPTURED → MUTATED → SEALED
    └──────────────→ DISCARDED_NO_MUTATION
```

The unique key is `(change_set_id, canonical_relative_path)`. The preimage is an
insert-only row: first successful capture wins. Postimage data is a separate insert-only
row written at seal. Neither API exposes preimage update/delete.

```text
start A → write B → write C → seal C → restore A
start A → write B → write A → seal no-op entry → no restore action
```

A tool that captures and then fails before writing leaves `CAPTURED`. A later tool in the
same turn may use the same preimage and proceed; the failed operation is evidence, not a
second preimage. At seal, `CAPTURED` entries with no observed filesystem change become
`DISCARDED_NO_MUTATION` and release their blob refs.

Operation evidence is append-only and bounded to 64 records per path. Each record carries
tool name, tool-call id, internal tool round, operation kind, outcome, and intermediate
kind/mode/size/digest. Overflow makes the set `INVALID`; it never silently truncates the
history used to prove coverage.

## 4. Workspace authority and path safety

### 4.1 Supported root

P0–P5 require all of the following:

1. active provider kind is `WS_PROVIDER_SHARED`;
2. `require_session_worktree` is satisfied;
3. the root is the worktree recorded for this session in `session_state_worktrees`;
4. the current caller owns `server_sessions.principal` for the session;
5. the root is opened as a directory fd and its identity matches the sealed tuple.

The sealed root tuple contains the session worktree mapping reference, `st_dev`, `st_ino`,
Git common-dir/worktree identity, and launch/HEAD fingerprint already used by worktree and
delegate drift checks. A display-only root hint may be stored, but an absolute path is
never restore authority.

Mirror, detached, and container providers are `coverage=unsupported_provider` in the
first release. This also fixes the current bug where capture can read the server path
while the mutation executes elsewhere.

### 4.2 One no-follow resolver

One workspace module, used at capture, seal, preview, preflight, apply, and compensation,
resolves a normalized relative path from the pinned root fd:

- reject empty, absolute, `.`/`..`, NUL, control, and over-limit components;
- walk every parent with fd-relative `openat`/`openat2`, `O_DIRECTORY|O_NOFOLLOW`, and
  `RESOLVE_BENEATH|RESOLVE_NO_MAGICLINKS|RESOLVE_NO_SYMLINKS` where available;
- operate on the leaf with `fstatat(..., AT_SYMLINK_NOFOLLOW)` and fd-relative
  `renameat`/`unlinkat`; never reopen a validated string path;
- recheck root identity and the resolved parent immediately before each commit step;
- refuse cross-device targets and root/provider capability drift before the first write.

Symlinks, directories, devices, sockets, FIFOs, hard-link topology, ACLs, and xattrs are
unsupported in P0–P5. Encountering one marks that entry refused; protected mode blocks
the mutation. This is intentionally smaller than a misleading partial metadata restore.

Named adversarial tests include parent replacement, leaf symlink swap, worktree move,
removed/recreated root, `..`, bind-mount/root retarget, link-target swap, and cross-mount
rename. Each must fail before any target write.

## 5. Capture integration

### 5.1 Native mutation wrapper

Add one internal wrapper below the existing authorization guards and above the provider
write:

```text
mutation_prepare(resolved_target, optional_current_bytes, operation)
  → validate bound execution/root/provider
  → refuse an active restore lease or unacknowledged incident for this root
  → capture insert-only first preimage durably
  → return mutation token

perform existing provider mutation

mutation_finish(token, tool_outcome)
  → lstat/digest observed state
  → append operation evidence
```

The wrapper is not a public MCP/domain operation. External callers cannot capture an
arbitrary path or bypass tool guards.

Integration points:

- anchored `edit_file`: reuse the exact current bytes already read by
  `tool_edit_file_anchored()` after anchor validation and before `write_all`; this avoids
  a second read and binds capture to the bytes actually edited;
- legacy string edit: refactor the existing read/plan/write sequence to pass its current
  buffer through the same wrapper;
- `write_file`: resolve once, capture current regular/absent state, then call the existing
  provider write;
- dry-run: no capture and no mutation evidence;
- failed guard/validation: no set entry;
- failed provider write: evidence records failure; the unchanged entry is discarded at
  seal unless a later operation mutates it.

The existing read-only-delegate, parent-worktree, and session-isolation checks remain
before capture. The wrapper cannot turn a denied operation into an allowed one.
Immediately before its provider write it rechecks that no exclusive rewind lease was
issued after prepare, closing the race between restore admission and a newly starting
turn.

### 5.2 Multi-call and future multi-file edits

The current native surface mutates one file per call. If the shipped anchored-edit
proposal's future multi-file form is implemented, it must prepare all targets before any
write and finish them under one tool-call id. Failure to prepare any target rejects the
whole tool call.

No new delete, rename, chmod, or directory tool is introduced here. A future native
mutation tool must register its rewind behavior in the tool registry and pass a
conformance test before protected mode can classify it as covered.

### 5.3 Shell and Git honesty

An arbitrary shell invocation can mutate files without passing the native wrapper. For a
managed Git worktree, Aimee may compare bounded pre/post status fingerprints for evidence,
but command parsing is not a preimage mechanism.

In P0–P5, execution of a shell or direct Git mutation marks the entire turn
`coverage=observe_only_shell`. Even native entries from that turn are previewable evidence
only and are not offered as “restore turn.” This avoids presenting a partially captured
turn as complete.

The same rule applies to any write-capable tool-registry entry that does not declare and
pass the native rewind conformance contract, including filesystem-mutating MCP tools.
Provider-CLI and tmux-CLI agents can edit through their own harness rather than
`agent_tools_dispatch.c`; their turns are `coverage=observe_only_external_harness` until
that harness reports verified preimages through a future capability contract. Coverage is
derived from the executed tool identities and side-effect metadata, not from which files
happen to appear in Git status afterward.

A Git object may replace a stored blob only when the entry proves the start state matched
that exact object. Otherwise the captured first preimage is authoritative (§0).

## 6. DB1 ownership, schema, and storage

### 6.1 Additive v2 tables

`src/db1/schema.sql` gains new versioned tables; `schema_data.h` is regenerated through
the existing build rule. The legacy tables remain unchanged and read-only during the
transition.

The conceptual ownership is:

- `rewind_change_sets`: execution/session/worktree identity, state, coverage, counts,
  schema version, manifest hash, timestamps;
- `rewind_preimages`: insert-only relative path, kind/mode/size/digest/blob ref,
  sensitivity ruleset version;
- `rewind_blob_refs`: the mutable active-retention edge from a set/entry to a blob;
  expiry removes this edge only after the set becomes non-restorable, so immutable
  preimage evidence can retain its digest and historical blob id without pinning bytes
  forever;
- `rewind_postimages`: insert-only sealed kind/mode/size/digest;
- `rewind_operations`: bounded append-only tool evidence;
- `rewind_restore_runs` and `rewind_restore_steps`: preview binding, durable apply journal,
  compensation refs, per-step state, terminal outcome;
- `rewind_worktree_leases`: DB1-backed root-identity lease with owner, fencing token,
  heartbeat, and expiry, so a daemon replacement or offline maintenance process cannot
  overlap restore with capture;
- `rewind_incidents`: one-way security/integrity quarantine and acknowledgement state;
- `rewind_blobs`: per-principal namespace, digest, size, refcount, lifecycle state.

DB constraints/triggers prevent UPDATE/DELETE of sealed preimages, postimages, and
operation evidence. State transitions use compare-and-swap updates. The implementation
plan must include upgrade-from-current and downgrade/read-compatibility tests; it may not
rely on the current best-effort ignored-`ALTER TABLE` loop for integrity-critical DDL.

### 6.2 Blob store

Preimage bytes move out of unbounded inline SQLite blobs into a DB1-owned store beneath
`$AIMEE_HOME/rewind/blobs`, following Aimee's existing content-addressed-store durability
pattern:

- per-principal namespace; no cross-principal deduplication or equality oracle;
- SHA-256 content identity, opaque refs at external surfaces;
- `0700` directories, `0600` regular files, `O_NOFOLLOW`;
- unique temp write → file `fsync` → atomic rename → directory `fsync`;
- DB ref committed only after the blob is durable;
- blobs with an active `rewind_blob_refs` edge are never deleted;
- oldest eligible sealed sets expire under both count and byte ceilings;
- manifest/audit rows survive blob expiry.

Expiry is ordered: compare-and-swap the set from restore-eligible to `EXPIRED`, remove its
active ref edges in the same DB1 transaction, then let asynchronous GC delete now-unreferenced
blob files after a grace period. A preimage row is never rewritten merely because its
retained bytes expired.

Garbage collection is bounded and asynchronous. If it cannot make space, protected
capture blocks before mutation with `store_budget_exhausted`; it does not delete a live
ref or silently degrade to partial coverage.

### 6.3 Sensitive content

P0–P5 expose one sensitivity posture only: `refuse`.

Before persistence, the existing sensitivity detector classifies the candidate bytes
under a recorded ruleset version. A detected secret or a result the detector cannot
safely classify blocks protected capture and therefore blocks the mutation. `warn` cannot
override this rule. No plaintext sensitive preimage enters the rewind store, preview,
logs, metrics, traces, or exports.

Encrypted sensitive capture is post-P5 and requires a separately approved envelope
contract and conformance suite. It is not a selectable dormant option in this proposal.

## 7. Seal, preview, and restore

### 7.1 Seal at the server-owned turn boundary

After `chat_stream_worker()` returns but before `presence_emit_turn_done()` /
`presence_turn_release()` publishes completion:

1. wait for every prepared native mutation token to reach a terminal tool outcome;
2. resolve every entry again through the pinned root fd;
3. insert final postimage tuples `(kind, mode, size, SHA-256)`;
4. discard unchanged captured-only entries;
5. derive coverage from tool evidence and provider kind;
6. hash the canonical immutable manifest and transition `OPEN → SEALED`, `ABORTED`, or
   `INVALID`;
7. publish a small `turn_changes` presence event containing only set id, counts, coverage,
   and state; the durable manifest remains in DB1.

Cancellation does not imply rollback. A cancelled turn with verified completed mutations
seals as `SEALED_INCOMPLETE`; a cancelled turn with none becomes `ABORTED`.

### 7.2 Preview contract

Preview re-authorizes session ownership and root identity, then returns:

- session, presence turn, state, coverage, and worktree display identity;
- per path: operation summary and pre/post/current kind, mode, size, digest;
- bounded inverse unified diff from `diff.c` for eligible non-sensitive text;
- binary/oversize metadata only;
- current-state conflicts and estimated writes/deletes.

The server persists a random, short-lived preview id bound to:

```text
(requester principal, set id, manifest hash,
 current-state root hash, expiry)
```

Apply recomputes all fields. Any difference returns `preview_stale`; it never writes.

### 7.3 No force restore in P0–P5

An entry applies only when the current tuple exactly matches the sealed postimage tuple.
Any drift returns `cas_conflict` for the complete requested set before the first write.

There is no `--force`, MCP force field, hidden admin bypass, or autonomous override in
P0–P5. A future force flow depends on the common governance `require_approval` surface,
distinct human approval, current-state binding, and step-up authentication; it requires a
separate proposal and security review.

### 7.4 Journaled compensating restore

The filesystem cannot provide one atomic commit across several paths. The product must
say **journaled compensating restore**, not atomic restore.

Before the first target write:

1. acquire the DB1-backed per-worktree lease, retain its fencing token through every
   journal update, and prove no turn is active for the session/worktree;
2. authorize and no-follow-resolve every manifest path;
3. verify root/provider identity, blob integrity, free space, same-device constraints,
   and compare-and-swap tuples;
4. capture the current state of every manifest target as a terminal compensation set;
5. durably write `rewind_restore_runs` and every ordered step as `STAGED`;
6. prepare same-filesystem, fd-relative temp objects and `fsync` them.

Commit processes paths in deterministic order, using fd-relative rename/unlink, with file
and parent-directory `fsync` after each rename group. Each completed step is journaled.
The fencing token is revalidated immediately before every target mutation; an expired or
superseded owner cannot continue applying after daemon replacement.
Only after all filesystem steps land does one DB1 transaction mark the operational result
`RESTORED_AUDIT_PENDING` and enqueue a stable audit event id. The common audit bridge then
appends that event to its separate sink and a final DB1 compare-and-swap marks `RESTORED`.
This does not pretend SQLite and the WORM/file audit sink share a transaction. Because the
current append-only sinks have no idempotency key, crash recovery is explicitly
at-least-once: doctor retries the same event id and readers collapse duplicate terminal
events by that id. A future authoritative sink may add native deduplication or require
compensation on audit failure; this proposal does not claim cross-store exactly-once.

On a normal apply failure, the same journal drives compensation in reverse order. A crash
leaves `RESTORE_IN_PROGRESS`; doctor can deterministically resume compensation from the
durable step states. Compensation sets are inspectable but never appear as rewind
candidates and cannot themselves be restored.

If compensation cannot converge, transition to `RESTORE_INCIDENT`, freeze new mutations
for the worktree at `mutation_prepare`, emit a security/operations audit event, and require
operator acknowledgement after repair. Never return green success with errors.

## 8. Configuration and compatibility

Evolve the existing nested section:

```yaml
rewind:
  mode: off | observe | protected
  max_entry_bytes: 8388608
  max_set_bytes: 67108864
  max_store_bytes: 536870912
  keep_sets: 50
```

- `off`: no v2 capture.
- `observe`: record bounded operation/coverage metadata without preimage bytes; never
  restorable.
- `protected`: block a covered native mutation unless its safe preimage is durable.

The default remains `off`. Existing `auto_snapshot: true` maps to a deprecated
`legacy-observe` posture with a startup/config warning; it does not silently opt a tenant
into new blocking behavior or protected v2 storage. The operator must explicitly write
`mode: protected`.

Nested config continues through `config.h`, `config_sections.c`, `config_save.c`, schema
validation, `test_config_surface`, and generated configuration docs. If the pending route
or config descriptor refactors land first, implementation uses their new source of truth;
this proposal does not fork them.

### 8.1 Legacy containment

P0 disables `db1_fsnap_restore()` at every user-facing path. Legacy snapshots lack root
identity, safe relative paths, postimage tuples, and coverage, so they cannot be migrated
into trustworthy restorable sets.

Legacy rows remain listable/exportable as `legacy_unverified` for manual recovery and
doctor classification. They are never returned as one-click restorable through CLI, MCP,
or webchat. The unsafe `--no-restore` conversation label is removed; if DB1 window pruning
remains useful, it must move to a separately named context-maintenance command.

The compatibility adapter is read-only and cannot append to or join a v2 set. Removal of
the legacy restore surface returns a typed `legacy_restore_removed`, never silent success.

## 9. One domain service, three adapters

### 9.1 Internal domain API

Place orchestration in a dedicated rewind module and persistence behind DB1 APIs. Public
operations are limited to:

- list/show by authorized session or turn;
- preview;
- apply a valid preview;
- prune/verify/repair/quarantine.

`capture-first` remains internal to the mutation wrapper. Tests receive an explicit
test-only fixture seam rather than a production callable capture API.

All layers share a closed error taxonomy, including:

`owner_mismatch`, `unsupported_provider`, `unsupported_object`, `capture_failed`,
`sensitive_refused`, `store_budget_exhausted`, `coverage_observe_only`,
`root_identity_changed`, `path_resolution_denied`, `preview_stale`, `cas_conflict`,
`restore_busy`, `compensation_failed`, `legacy_restore_removed`, and `integrity_invalid`.

### 9.2 Server and CLI

The server is authoritative. Add `/v1` operations for list/show, preview, restore, and
focused doctor actions. Every request obtains the principal from the authenticated server
context; body/query principal fields are rejected.

Route admission is explicit: list/show/preview and report-only doctor require
`CAP_SESSION_READ`; restore requires `CAP_TOOL_EXECUTE`; repair, quarantine, and incident
acknowledgement require `CAP_SESSION_ADMIN`. The handler still enforces exact session
ownership and worktree identity after the coarse capability check.

`cmd_rewind.c` becomes a thin adapter through the existing `cli_v1` dispatch. A local
server/UDS uses the same route and authorization logic; remote clients no longer open
their own DB1. Human and JSON printers consume the same response schema.

Route work follows the repository's current source-of-truth at implementation time:

- if the route-descriptor proposal has landed, add descriptor rows and regenerate;
- otherwise add `g_v1_routes[]` rows, CLI marshal/printer mappings, and run the existing
  route/conformance generators and drift scripts.

### 9.3 MCP

MCP wrappers call the same server operations. They never expose raw preimage blobs and do
not provide autonomous restore by default. `preview` is read-only; `restore` is a governed
write operation and must pass the execution's capability policy and normal tool
guardrails.

### 9.4 Webchat

Replace the current rewind stub only for filesystem changes. It must not reuse or imply
the adjacent thread/branch stubs.

- `turn_changes` enables a per-turn “Changes” affordance.
- The webchat relay binds the authenticated cookie principal and session id; it rejects
  caller-supplied identity.
- The UI fetches durable preview data for the complete set and sends the short-lived
  preview id for confirmation. Path-selected restore is not exposed in P0–P5.
- Coverage, conflict, expiry, and restore-incident states are first-class; no partial
  outcome becomes a green toast.
- Reconnect queries DB1 by session/turn; correctness never depends on presence-ring
  retention.

## 10. Audit, observability, and doctor

### 10.1 Audit integration

Emit bounded events at set seal, preview confirmation, restore intent, restore completion,
compensation, incident, repair, and expiry. The common audit bridge dual-writes according
to the active governance posture; rewind code does not call independent sinks with
different schemas.

Safe fields are actor role/principal, session/turn ids, worktree identity hash, set id,
manifest/preview hashes, entry-path hashes, counts, coverage, reason enum, outcome, and
compensation id. Never emit raw paths, content, diffs, detected secret values, or blob
refs.

When the pending identity proposal lands, its per-agent principal and delegation chain
populate the existing actor fields. When attestable enforcement makes WORM authoritative,
the same rewind event schema becomes chain-authoritative without a rewind migration.

### 10.2 Metrics

Metrics are aggregate counts/bytes/latency only:

- capture attempts, success, and fixed refusal reasons by native tool class;
- native coverage vs `observe_only_shell` / unsupported provider;
- first-preimage reuse;
- raw, deduplicated, referenced, expired, and orphan blob bytes;
- open/sealed/incomplete/invalid/expired sets;
- previews, stale previews, conflicts, restores, compensation attempts/failures;
- p50/p95/p99 capture latency and write overhead ratio;
- doctor findings, quarantines, repair outcomes, and incident acknowledgements.

Reasons are a fixed enum plus `other`; any `other` blocks rollout until classified.

### 10.3 Doctor integration

`aimee doctor` includes a rewind summary. `aimee rewind doctor --json` exposes the same
findings with optional explicit `--repair`, `--quarantine`, and `--ack-incident` actions;
default is report-only and every mutation supports dry-run.

Classifications include:

- `healthy`;
- `recoverable_open` or `entry_count_mismatch`;
- `missing_blob`, `hash_mismatch`, `orphan_blob`, `orphan_entry`;
- `wrong_schema`;
- `legacy_unverified`;
- `root_identity_changed`, `path_resolution_denied`;
- `restore_in_progress`, `compensation_required`, `restore_incident`.

Repair may rebuild refcounts/indexes from immutable manifests, expire a set with missing
bytes, or resume a journaled compensation. It may not invent bytes, rewrite a digest,
guess a path, promote partial coverage, or silently delete evidence. Security path
violations enter one-way quarantine and require acknowledgement before the worktree
unfreezes.

Startup performs only a bounded read-only check of open/restore-in-progress rows and emits
health state. Deep scans and repairs remain operator initiated, keeping DB1 initialization
predictable. The appliance recovery runbook consumes these classifications rather than
duplicating them.

## 11. Implementation sequence and dependencies

These slices state what must become true; the implementation plan owns exact signatures
and SQL.

### P0 — Contain the unsafe legacy path

- change the legacy capture regression from latest-wins to first-wins;
- propagate capture outcome instead of ignoring it;
- refuse capture when the active provider is not shared;
- hard-disable user-facing legacy restore and remove the misleading conversation option;
- add owner/session checks to every legacy list/export operation;
- keep all new behavior default-off.

P0 tests update `test_record_overwrite_within_snapshot` to expect the original bytes and
assert legacy restore returns `legacy_restore_removed` without writing.

### P1 — V2 storage and turn binding

- additive DB1 tables, immutable rows, blob store, budgets, state machine;
- bind presence turn id/principal/worktree before dispatch and seal before `turn_done`;
- retain internal tool round and tool-call evidence;
- support shared managed worktrees and regular/absent entries only;
- sensitivity refusal and observe/protected config parsing.

P1 does not expose restore.

### P2 — Native mutation coverage and preview

- move capture into the guarded anchored-edit, legacy-edit, and write-file seams;
- reuse anchored current bytes and `diff.c`;
- implement root-fd no-follow resolver, sealing, coverage truth, list/show/preview;
- mark any shell/git turn observe-only.

P2 is read-only at user surfaces.

### P3 — Journaled restore

- short-lived preview binding and exact CAS;
- per-worktree lock, durable restore/step journal, compensation sets;
- fd-relative staged apply, fsync ordering, incident freeze/recovery;
- no force path.

### P4 — Server, CLI, MCP, webchat, and audit parity

- server-authoritative `/v1` operations and generated conformance artifacts;
- thin `cmd_rewind.c` adapter and stable JSON/human printers;
- governed MCP adapters with no raw blob read;
- webchat `Changes` UI distinct from conversation branching;
- presence `turn_changes` notification and durable reconnect query;
- audit event parity across every surface.

### P5 — Doctor and measured rollout

- integrate focused findings into `cmd_doctor.c` and appliance recovery;
- GC, quarantine, compensation resume, alerts, and dashboards;
- observe corpus, protected opt-in canary, and rollout artifact.

### Post-P5, separately approved

- durable snapshot/atomic-replace capabilities for detached/container/mirror providers;
- native delete/rename/mode/symlink support;
- safe shell coverage beyond observe-only;
- encrypted sensitive capture;
- force restore with common governance approval and step-up authentication;
- delegate/workflow protection after their execution ids and worktree ownership are wired.

## 12. Acceptance gates

### 12.1 Correctness and lifecycle

1. Across internal tool rounds, `A → B → C` restores `A`; `A → B → A` seals no restore
   action; capture-then-tool-failure followed by a successful mutation still restores `A`.
2. Create/overwrite, empty, binary, oversize, unsupported kind, and 65-operation evidence
   cases produce the specified outcomes; overflow is `INVALID`.
3. A dry-run, denied guard, failed validation, or failed unchanged provider write produces
   no restorable mutation.
4. Cancellation with completed mutations seals `SEALED_INCOMPLETE`; no mutation becomes
   `ABORTED`; a crash leaves only doctor-classifiable states.
5. A set spans one presence turn and never absorbs another turn, delegate worktree, or
   workflow attempt.

### 12.2 Security and tenancy

6. Guessing a set/preview id cannot list, preview, or restore another principal's session.
7. Detached, container, and mirror turns cannot produce a restorable set in P0–P5.
   Provider-CLI/tmux-CLI and undeclared write-capable MCP tools are likewise observe-only.
8. Every named path/root adversarial case in §4.2 refuses before the first write.
9. Detected or unclassifiable sensitive bytes never enter DB/blob/log/preview/trace output;
   a versioned secret fixture/fuzz corpus enforces this.
10. Capability denial, session ownership denial, active-turn conflict, stale preview, and
    CAS drift each refuse the complete set before mutation.
11. No CLI, MCP, webchat, admin, or autonomous force field exists.

### 12.3 Restore durability

12. Crash injection before/after every blob, manifest, seal, journal, temp write, rename,
    step-state, filesystem-fsync, DB commit, compensation step, and audit boundary yields a
    deterministic doctor action and never false success.
13. Cross-device staging refuses in preflight; it never becomes a partial restore.
14. Compensation sets are terminal/non-restorable; compensation failure freezes the
    worktree at the native mutation seam until repair and acknowledgement.
15. Restart during `RESTORE_IN_PROGRESS` resumes compensation from journal state without
    double-applying a completed step; a stale lease owner cannot write after a newer
    fencing token is issued.
16. Restart during `RESTORED_AUDIT_PENDING` retries the same stable audit event id,
    downstream views collapse any duplicate append, and no filesystem step is reapplied.

### 12.4 Integration parity

17. CLI JSON, MCP, server, webchat, and doctor expose identical state, coverage, reason,
    conflict, and outcome enums.
18. `turn_changes` ordering is before `turn_done`; reconnect correctness comes from DB1,
    not ring retention.
19. Route generation/conformance, config round-trip/surface, schema convergence, and
    OpenAPI/reference generation gates are green.
20. Legacy rows remain exportable but every restore surface returns
    `legacy_restore_removed` and writes nothing.
21. Overall doctor and the appliance recovery runbook agree on the same rewind health
    classifications.

### 12.5 Performance and rollout

22. On a committed versioned corpus, protected capture adds <5% median and <15% p95
    latency to eligible native writes; p99 and GC backpressure are reported, not hidden.
23. Observe mode classifies ≥99% of native write/edit calls. Shell/git, external harnesses,
    undeclared write-capable tools, and unsupported providers are reported separately and
    cannot be counted as covered.
24. A 30-day opt-in canary has zero cross-worktree writes, zero false-safe partial sets,
    zero missing-preimage restores, and zero uncompensated incidents.
25. A versioned rollout artifact contains corpus hash, latency distribution, coverage by
    tool/provider with refusal reasons, conflict rate, compensation count, incident count,
    explicit tenant list, and opt-in mechanism.

## 13. Risks and mitigations

- **False confidence:** complete/observe-only/unsupported/invalid are closed enums; only
  complete native sets are restorable.
- **Restore races:** session/worktree lock, root-fd resolution, short-lived preview, and
  exact postimage CAS.
- **Crash-partial filesystem state:** durable per-step journal and terminal compensation
  sets; no claim of filesystem atomicity.
- **Secret duplication:** refuse detected or unclassifiable sensitive content in P0–P5;
  encryption is not a dormant flag.
- **Store growth:** per-principal content addressing, byte + count ceilings, asynchronous
  GC, and block-before-mutate backpressure.
- **Architecture duplication:** existing turn, workspace, provider, capability, audit,
  route, config, and doctor owners remain authoritative.
- **Git confusion:** UI says “restore Aimee-captured file changes from this turn,” never
  “reset branch” or “undo commit.”

## 14. Decision requested

Approve the narrowed Aimee-native design and P0–P5 sequence. The implementation plan is
owned jointly by Execute and Enforce, with Review signing off each slice's acceptance
mapping before implementation.

No slice, including P5, authorizes flipping `rewind.mode` from `off` to `protected` for
any tenant without a separately approved rollout artifact satisfying §12.5 against
measured data. Post-P5 capabilities in §11 require their own approval and cannot be
silently absorbed into this proposal.

## 15. Roundtable disposition

Revision 4 completed final review in run
`oprun_g6a5fdfb91049914d_1784668448_7`. The aggregate found no issues, classified the
proposal as aligned with the request, and retained zero findings after replay
verification. The run completed with 14 of 20 participants; six participant failures
marked the run degraded, but all 51 candidate objections were rejected during replay and
no blocking or non-blocking item survived. This section records the review result only;
it does not alter the reviewed design.
