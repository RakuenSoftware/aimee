# Proposal: webchat project lifecycle — org-scoped clones and true delete/purge

- **State:** PENDING — three slices, ordered, each independently deployable.
  Slice 1 changes the clone layout/identity (including the minimum GUI/API
  compatibility so nothing regresses mid-feature), slice 2 adds the
  delete/purge plumbing, slice 3 finishes the GUI. Builds on PR #1332 (web-GUI
  clones push to aimee-kb via `/v1/code/scan`), which is why project
  *identity* now matters end-to-end: the name the clone path chooses becomes
  the kb's project key for embeddings, code units, and curator vectors.
- **Author:** JBailes
- **Date:** 2026-07-14 (rev 5 — round 4 blocker: a deployment-global
  key→remote+holders REGISTRY, mutated only under the lifecycle lock,
  closes the publish→first-scan aliasing window, makes last-holder O(1)
  without cross-tree enumeration, and reduces conflict disclosure to a
  generic "key taken". Plus: fence lifecycle table with heartbeat +
  configurable TTL + mismatch-no-op finalize, retained-path skips the
  shared lexical delete and omits fence_generation, explicit relay field
  contract, flat-fallback ref spelled out, force/fence interaction
  clarified. rev 6 — round 5 blocker: clone registration/publication is a
  single lock-held transaction with rollback of the registry increment on
  any pre-publish failure; registry entry writes are atomic
  (tmp+fsync+rename) and startup rebuild counts published clones only)
- **Charter roles:** Execute (clone/delete filesystem + git operations under
  the webuser scope), Persist (purge is ordered, fenced, idempotent, and
  audited — no silently orphaned rows), Review (delete is destructive:
  attested same-principal only, typed confirm in the GUI, structured audit
  line on every attempt including aborts).

## Thesis

The webchat GUI can create projects but never destroy them, and it flattens
every clone into one namespace.

1. **No delete.** There is no project-delete surface anywhere — not in the
   GUI, not in `/v1`, not in the CLI. `rh_workspace_remove` (`DELETE
   /v1/workspaces/{id}`) deregisters a *workspace config entry*; nothing
   removes a cloned webuser project. Once a repo is cloned and (since PR
   #1332) ingested, its clone dir, server-local lexical index rows, and kb
   artifacts live forever. An operator who cloned the wrong repo — or 22 of
   them — has no recourse but SSH.

2. **Org collision.** `git_project_clone` derives the project name from the
   URL basename and discards the owner (`derive_name`,
   `src/server/git_project.c:24-52`): `RakuenSoftware/foo` and `jbailes/foo`
   both target `webusers/<user>/foo` — the second clone fails, and the kb
   would merge two different repos under one key if it didn't.

## Design

### Identity model (used by all slices)

- **Project ref** `<org>/<repo>` — at most two components, the on-disk layout
  under the webuser root and the display identity. Legacy flat projects have
  a one-component ref (`<repo>`).
- **Canonical remote** — the clone URL normalized to a credential-free form
  at clone time: scheme (or `ssh`) + host + path, `.git` stripped, userinfo
  (`user:pass@`) and query/fragment REMOVED. It is written to a per-clone
  sidecar `<project>/.aimee/remote` (atomically, before publish — below) and
  echoed into the kb project row at scan time. Raw `remote.origin.url` is
  never surfaced in API responses, conflict errors, or audit lines — only
  the credential-free form. The sidecar is a CACHE of the clone's git
  config: every conflict/holder check re-derives the value from `git config
  --get remote.origin.url` (output length-capped, non-zero exit → "unknown")
  and refreshes the sidecar, so a user-run `git remote set-url` is honored
  rather than spuriously 409ing. A holder whose remote is UNKNOWN
  (corrupt/partial clone, unreadable config) is conservatively treated as a
  SAME-remote holder at delete time — kb retained, loud log — because stale
  kb rows are recoverable via the purge route while a wrong purge is not.
  For a flat legacy clone without a sidecar the same git-config fallback
  applies.
- **kb project key** — the project ref, verbatim. Fixed at clone time; the
  scan path never re-keys. Legacy flat projects keep their bare key; their
  migration is delete + re-clone.

**Org derivation.**

- Bulk (`/v1/workspace/clone-org`): the `owner` field already in the body
  (currently parsed and dropped, `rh_workspace_clone_org`,
  `server_http_routes.c:1308`).
- Single (`/v1/workspace/clone`): the owner path of the clone URL — the URL
  path minus the trailing repo segment, `.git` stripped; host dropped from
  the layout. A multi-segment owner path (GitLab subgroups,
  `gitlab.com/group/sub/repo`) does NOT get a joined org — joining is lossy
  — it bails to the flat fallback (the canonical remote still dedupes
  correctly); the user can pass an explicit `org` override to place it. Each
  candidate org is sanitized to the `ws_scope_name_valid` alphabet (invalid
  bytes → `-`, collapse runs, trim), re-validated by `ws_scope_name_valid`
  including the 64-byte cap, and must be non-empty — else the flat fallback
  applies. The sanitized org is computed ONCE and that single value is used
  for the conflict checks, the registry, the layout, and the kb key — no
  code path re-derives it from the URL, so a URL crafted to normalize
  differently at different sites cannot split identities. An optional `org`
  body field overrides derivation (validated identically; it cannot alias
  another remote — see the conflict rule). When the flat fallback triggers
  on a multi-segment owner, the response includes the sanitized candidate
  org segments and instructs the caller to pass an explicit `org` override
  (derived from the caller's own URL — no cross-principal data).
- Fallback: no derivable owner (or multi-segment owner) → flat
  one-component ref = the trailing repo segment exactly as today (owner
  discarded from the ref; the canonical remote still dedupes); kb key is
  the bare name; the sidecar is still written and the key is still
  registered.

**Key registry (authoritative, closes the publish→first-scan window).** A
deployment-global registry maps each live key (ref) → `{canonical remote,
holder count}`: one entry file `<webusers_base>/.registry/<sha256(key)>`,
mutated ONLY under the key's lifecycle lock. Clone REGISTERS before publish
(create entry, or holders+1 if the remote matches; remote mismatch → 409);
delete decrements under the same lock and removes the entry when it hits
zero — that zero IS the last-holder signal (no per-delete filesystem
enumeration across webusers, no timing side channels, O(1)). The registry
is rebuilt from a filesystem walk (sidecars/git config) at startup, so
drift self-heals. The kb row's remote remains a defense-in-depth
cross-check at scan time.

**Conflict rule (same key, different remote).** Under the lifecycle lock:
the caller's own tree is checked first (409 on plain existence, echoing
only the CALLER's credential-free remotes), then the registry (mismatch →
a GENERIC 409 "this ref is bound to a different remote" that does NOT echo
the other remote or say who holds it — the only disclosure is that the key
is taken, which the registry design accepts and documents). This applies
equally to explicit `org` overrides and to flat fallbacks colliding with
existing flat clones. A holder that later runs `git remote set-url`
diverges from its registration; the divergence is honored on that holder's
own re-clone checks (sidecar re-sync) and logged loudly if seen at delete
time, and the conservative retain rule applies.

**Flat/org namespace conflict.** Flat projects and org dirs share the first
path level. Creation keeps them disjoint (checked under the same lock):
cloning into org `acme` → 409 if a flat project `acme` exists; a flat clone
`acme` → 409 if an org dir `acme` exists. The lister distinguishes published
entries structurally (first-level dir with `.git` → flat project; without →
org dir) — unambiguous because unpublished clones are never visible (atomic
publish, below).

### Lifecycle lock (keyed by FIRST component)

All create/delete decisions are serialized deployment-wide by a lock file
`<webusers_base>/.locks/<sha256(first component of ref)>` taken with
`flock(LOCK_EX)`. Keying on the FIRST component — the org for `acme/foo`,
the bare name for flat `acme` — means a flat clone of `acme` and an org
clone of `acme/foo` take the SAME lock, so the flat/org namespace conflict
checks, publication, deletion, and org-dir pruning are all mutually
serialized (this was the round-3 gap: ref-keyed locks let `acme` and
`acme/foo` race). The coarser granularity (all repos under one org
serialize) is acceptable: these are interactive clone/delete operations.
Clone holds the lock from the conflict checks through registry increment
and publish; delete holds it from the registry decrement through filesystem
removal and org-dir prune. A
clone cannot appear between "count holders" and "purge"; two concurrent
final deletes serialize; org pruning under the lock cannot race a clone
into the same org (Acceptance 4 includes the concurrent flat-`acme` vs
org-`acme/foo` test).

### Slice 1 — org-scoped clone layout and identity (+ GUI/API compatibility)

**Atomic, fd-relative creation.** Under the lifecycle lock:

1. open the webuser root `O_NOFOLLOW|O_DIRECTORY` (existing
   `ws_scope_user_root` handle);
2. `mkdirat(rootfd, org)` (EEXIST ok), reopen `openat2(rootfd, org,
   RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|O_DIRECTORY)` — openat2 with these
   resolve flags is a HARD requirement with a startup feature check that
   fails closed: on kernels without openat2 (≥ 5.6) the ENTIRE webuser
   project surface (clone, delete, list, git-ops path resolution, sidecar
   reads) disables itself with a logged error — every directory-open in this
   design uses the same gated helper, not just clone creation; `fstat`
   confirms directory + expected owner (the daemon uid — all webuser trees
   are daemon-owned; webusers are principals, not OS users);
3. `mkdirat(orgfd, ".tmp-<repo>-<pid>")`, open it the same way, and run
   `git clone` with the pinned directory as git's destination CWD (clone
   into `.`), so git never resolves an attacker-influencable path; publish
   is conditional on git exiting 0 AND a resolvable `HEAD` in the temp dir
   (a partially-initialized repo never publishes);
4. write `.aimee/remote` (credential-free canonical remote) inside the temp
   dir, fsync;
5. publish: `renameat(orgfd, ".tmp-<repo>-<pid>", orgfd, "<repo>")` —
   atomic; a project is either fully present with its sidecar or invisible.
   The lister and all consumers skip dot-entries, so mid-clone state is
   never observable and the `.git`-presence structural rule is safe.
6. **registration/publication are one transaction under the still-held
   lock**: the registry increment (which happens after the conflict checks,
   before the clone work) is tracked by the operation, and ANY failure
   before a successful publish — clone error, sidecar write error, rename
   error — decrements it (removing the entry if it hits zero) under the
   same lock hold, then removes the temp dir. No code path releases the
   lock between increment and either publish or rollback, so phantom
   holders cannot exist. Registry entry files are themselves written
   atomically (write `<entry>.tmp`, fsync, rename), and the startup rebuild
   reconciles any crash-window drift (a crash between increment and publish
   leaves an entry the rebuild corrects, since rebuild derives counts from
   published clones only).

**Validator.** `ws_scope_name_valid` stays byte-for-byte the
single-component guard (`[A-Za-z0-9._-]`, first char alnum, ≤ 64 bytes). New
`ws_scope_project_ref_valid(buf, len)` — takes an explicit length, rejects
embedded NUL by byte-scan — is the ONLY function that ever accepts `/` in a
project reference. Rules: total ≤ 129 bytes; at most one `/`; zero `/` →
whole ref passes `ws_scope_name_valid`; one `/` → both segments non-empty
and independently pass `ws_scope_name_valid`. This inherits every existing
rejection (`..`, empty segments, leading dots, absolute paths, deeper
nesting, control bytes). **No new character set is introduced**: each
component is validated by `ws_scope_name_valid` unchanged, so non-NUL
control bytes (0x01–0x1F, 0x7F) and over-long UTF-8 sequences are rejected
exactly as today (unit-tested per component position).
`ws_scope_project_path` / `ws_scope_open_project` accept two-component refs;
`ws_scope_open_project` opens component-by-component with
`O_NOFOLLOW|O_DIRECTORY`.

**Listing & API.** `git_project_list` walks two levels per the structural
rule. `/v1/workspace/projects` returns, per project: `ref`, `org` (may be
empty), `name`, `remote` (credential-free). The legacy `projects: [string]`
array stays populated with refs — existing consumers parse an array of
strings and ignore unknown sibling fields (additive JSON; no version
negotiation needed). **Route audit:** no existing `/v1/workspace/*` or
`/api/git/*` route carries a project identifier in a path segment today —
`/v1/workspace/git`, `/v1/workspace/session-dir` (and their webchat relays)
take `project` in the POST body, and `/v1/workspace/projects` takes none —
so accepting refs requires only validator swaps (`ws_scope_project_ref_valid`)
at those body-parsing sites, and the new delete route (slice 2) is also
body-carried. Nothing needs `%2F` in a URL path.

**GUI compatibility in this slice:** Projects page and pickers render refs;
already-cloned detection compares canonical remotes (fixes the bare-name
`projects.includes(repo.name)` false positives across orgs).

### Slice 2 — delete/purge plumbing

**kb: one purge route.** `POST /v1/maintenance/purge-project {project,
generation}` — same auth boundary as the other kb maintenance routes
(bridge-network placement + kb bearer when configured).
Writer→store→delete matrix (the fan-out, in order):

| writer (ingest path) | store | delete primitive |
|---|---|---|
| `/v1/ingest` workers | kb service chunks | `db2_kb_service_clear_project` (exists) |
| `/v1/ingest` workers | file index (`kb_file_index`) | `db2_kb_file_index_delete_project` (exists) |
| curator drain | narrative/claim/entity vectors | `pgvec_kb_vector_delete_project` (exists) |
| curator code-unit drain | `code_embeddings` | `pgvec_code_delete_project` (exists, unwired) |
| curator code-unit drain | `curator_code_unit_vectors` | new (same pattern) |
| `/v1/code/scan` | canonical index (`projects` incl. the new `remote` column, `files`, cascade `file_exports`/`file_imports`/`terms`/`code_calls`) | new `db2_code_index_project_delete` |
| scan → curator queue | `kb_code_unit_jobs` | new per-project delete |
| pdf ingest | pdf vectors | `pgvec_kbpdf_delete_project` (exists) |
| near-dup indexing | minhash signatures | `db2_sketch_minhash_signature_delete_project` (exists) |

This table is the contract: every kb store the ingest path writes MUST have
a per-project delete primitive wired into this route, enforced by the
fixture test (Acceptance 7) that pushes a project through scan + ingest +
curator drain and asserts nonzero-then-zero per store. Fan-out continues
past individual store failures; the response is a per-store
`{count | {"error": …}}` map with `ok` only if all succeeded. Idempotent —
re-running on a purged key returns zeros. The route has no force flag: a
permanently failing store means the purge reports that store's error every
run and the runbook is operator-level DB intervention (documented in
`docs/OPERATIONS.md`); the server-side delete's `force` (below) governs
whether the *filesystem* proceeds regardless.

**Generation fence (covers the whole deletion interval).** The purge route's
first act is writing the fence: `kb_runtime_state` key
`project_purging:<key>` = `{generation, purge_id, ts}`. The fence is NOT
cleared by the purge route — it is cleared by an explicit `POST
/v1/maintenance/purge-finalize {project, generation, purge_id}` that the
*server* calls only after its local index + filesystem deletion completed
(or `purge-cancel` on abort). While fenced, EVERY writer for that key
aborts at the COMMIT POINT of its own transaction — a `SELECT` of the fence
key inside the transaction immediately before commit, aborting on presence:
`/v1/code/scan` in its indexing transaction (an in-flight scan that passed
enqueue pre-fence aborts at commit), ingest workers in each per-batch
transaction (a batch mid-flight past its enqueue check still cannot commit),
the pdf and near-dup writers in their insert transactions, and the curator
drain with its job-row re-check + BOTH vector inserts in ONE transaction —
a job claimed pre-purge can never re-insert into either vector store
post-purge. A stale fence (crash between purge and finalize) expires after
a configurable TTL (default 15 minutes, `kb_purge_fence_ttl_s`), and the
owning delete operation HEARTBEATS the fence (refreshes `ts`) between
phases, so a long purge/filesystem walk cannot outlive it; re-running
`purge-project` always writes a NEW generation over any existing fence
(`fence_replaced:true`), so a slow operator recovery cannot race the
original generation — writers only ever check "a fence exists".

Fence lifecycle table:

| event | actor | effect |
|---|---|---|
| `purge-project` | server delete step 4 (or operator) | writes fence `{generation, purge_id, ts}`; REFUSES (409) to overwrite a LIVE fence (heartbeat younger than 2× the heartbeat interval) unless `takeover:true`, whose response returns the displaced `{generation, purge_id}`; a displaced owner's later finalize/heartbeat mismatches and no-ops, so it cannot clear the new owner's fence |
| heartbeat | owning delete op between phases | refreshes `ts` |
| `purge-finalize {generation, purge_id}` | server after fs removal | clears fence iff BOTH match; mismatch → no-op, warning log, returns current fence state |
| `purge-cancel {generation, purge_id}` | server on abort after fence write | same match rule |
| TTL expiry | kb, lazily | fence ignored/garbage-collected; last-resort only |
| any writer commit | scan/ingest/pdf/near-dup/curator | SELECT fence inside its own transaction immediately before COMMIT; abort on presence (generation never read by writers) |

**server: webuser project delete.** `POST /v1/workspace/projects/delete
{ref, force?}` — capability `tool:execute`. **Principal contract: the ONLY
identity source is the attested `X-Aimee-Webuser` header** (set by the
server's token-gated identity layer; no body/query override, no fallback);
the ref resolves strictly under `webusers/<that principal>/…` and anything
else — including another webuser's ref — is a plain 404 (no existence
disclosure). Under the per-ref lifecycle lock:

1. authenticate the principal, validate the ref
   (`ws_scope_project_ref_valid`), and mint a `purge_id`;
2. audit intent FIRST — before any existence resolution — so a
   cross-principal or nonexistent ref still leaves a record:
   `audit webuser_project_delete_audit_v1 {schema_version:1, ts, purge_id,
   principal, ref, phase:"intent"}`;
3. resolve via `ws_scope_open_project` strictly under the caller's tree; on
   any resolution failure return 404 and audit `{phase:"aborted",
   reason:"not-found"}` — recording nothing about other principals' trees;
4. **holder decision (registry-based):** decrement the key's registry entry
   under the lock. Holders remain → skip kb purge AND the server-local
   index delete (`kb_status:"retained"`, `fence_generation` absent — the
   shared knowledge and shared lexical rows stay for the surviving
   holders). Count hit zero → last holder: call `purge-project` (fence
   carries this `purge_id` + generation); on kb-unreachable or any store
   error, ABORT with 503 and audit `{phase:"aborted", reason, kb_error}`.
   The registry decrement is reinstated ONLY if `purge-cancel` (same
   generation/purge_id) confirms the fence rollback; if the cancel itself
   fails, the fence stays, the decrement is kept, and the response is a
   terminal "purge committed but unfinished" error — re-running the delete
   converges. Nothing filesystem has been destroyed in either case.
   `force:true` proceeds anyway: if the kb was reachable enough to write
   the fence, writers are fenced; if fully unreachable, no fence exists and
   the audit says so (the kb is down, so no writer is committing either);
   the response and audit carry the FULL per-store succeeded/failed detail
   under `kb_status:"forced"` so the operator can re-run `purge-project` to
   convergence later. (The kb route itself has NO force flag — it always
   reports per-store outcomes and never partially proceeds silently.);
5. server-local lexical index removal (`db2_code_index_project_delete` on
   the server-side store) — these rows are keyed by project (shared across
   webusers, not per-caller), so they follow the kb decision exactly:
   deleted on `purged`/`forced`, kept on `retained` (including the
   conservative UNKNOWN-remote retain, where a loud log flags the possible
   dangling state for the operator);
6. filesystem removal: `unlinkat`-based walk rooted at the project fd; every
   directory descent is `openat(fd, name, O_NOFOLLOW|O_DIRECTORY)` —
   directory symlinks are unlinked as entries, never followed; file symlinks
   unlinked, never dereferenced; hard links affect this tree only;
7. prune org dir if empty (best-effort, see lifecycle lock);
8. `purge-finalize {project, generation, purge_id}` (clears the fence only
   when both match), audit done: `{schema_version:1, ts, purge_id,
   fence_generation, principal, ref, phase:"done",
   kb_status:"purged"|"retained"|"forced", kb: {store: count…},
   fs:"removed"}`. Every phase line of one operation shares the same
   `purge_id`, and the response body echoes it, so audit records, the fence,
   and the API outcome are provably the same event.

**webchat relay.** `POST /api/git/projects/delete`. Relay contract: the
accepted body fields are exactly `ref` and `force` — unknown fields are
rejected (400) so nothing can be smuggled to the server route; the
principal is bound EXCLUSIVELY from the authenticated webchat session
cookie (the same session auth as every `/api/git/*` handler), never from
the inbound request headers or body; the relay validates `ref` with
`ws_scope_project_ref_valid` BEFORE forwarding (uniform 400 on failure, no
existence disclosure; the server's not-found stays 404) and the server
re-validates everything.
Acceptance 5 pins that an inbound request carrying its own
`X-Aimee-Webuser` header or a principal-ish body field cannot influence
the attested identity.

### Slice 3 — GUI polish

- Projects page groups by org (legacy flat under "ungrouped"); per-row
  delete with typed-ref confirm ("removes the clone and all indexed
  knowledge"), calling the relay; `kb_status` outcomes and 503 aborts
  surface as actionable toasts (retry / force guidance).
- Org-repos picker shows per-repo cloned state via canonical-remote
  comparison and disables re-clone of an existing remote.

## Non-goals

- No migration of existing flat clones (delete + re-clone; bare kb keys
  until then).
- No org-level bulk delete (composes client-side).
- No change to detached/mirror providers or the `workspaces:` config
  surface.
- No per-webuser kb partitioning: the kb stays deployment-global; delete
  adds last-holder (by canonical remote) reference counting only.

## Risks

- **`/` in refs** — one validator owns `/`; refs never travel in URL path
  segments (route audit above); rejection-matrix unit tests.
- **Destructive delete** — same-principal scoping (404 outside), typed
  confirm, kb-first ordering with abort-on-failure, explicit audited
  `force`, structured audit on intent/abort/done.
- **Concurrent create/delete races** — per-ref lifecycle lock (atomic
  holder decisions), atomic rename-publish (no observable partial clones),
  full-interval generation fence with commit-point checks in every writer.
- **Purge completeness drift** — the matrix is a tested contract
  (Acceptance 7); new stores must ship a delete primitive wired here.
- **Cross-host/org aliasing** — clone-time sidecar comparison across all
  holders (kb-independent), 409 on same-key-different-remote, credential-free
  remotes only in responses/logs.
- **openat2 dependency** — fails closed with a clear startup log; the git
  surface (clone/delete) is unavailable rather than unsafe on pre-5.6
  kernels.

## Acceptance

1. Clone `github/RakuenSoftware/foo` and `github/jbailes/foo`: both exist
   under their orgs, ingest under distinct kb keys, git ops work on each.
2. Delete one as its only holder: clone dir gone, org pruned, every matrix
   store at zero for that key, the other project untouched. Delete while a
   second webuser holds the same canonical remote at the same ref:
   filesystem removed, kb retained, `kb_status:"retained"`.
3. kb down: delete without `force` → 503, audit `aborted`, clone intact;
   with `force` → filesystem removed, audit + response carry the full kb
   error; re-running `purge-project` after recovery zeroes the stores; a
   crash between purge and finalize leaves a fence that expires in 15
   minutes and a re-run converges.
4. Race tests: curator job claimed before purge cannot insert into either
   vector store after it (single fenced transaction); a scan or ingest batch
   in flight at fence time aborts at its commit point; clone vs final-delete
   of the same ref serialize under the lock (no purge-then-clone-visible
   interleave); concurrent flat `acme` clone vs org `acme/foo` clone
   serialize under the shared first-component lock with deterministic 409
   for the loser; concurrent clone during org prune leaves the org dir
   intact without erroring either operation.
5. Cross-principal: webuser A deleting webuser B's ref → 404, nothing
   removed, audit intent+aborted logged under A; the relay ignores any
   client-supplied principal.
6. Unit: `ws_scope_project_ref_valid` rejects `../x`, `a/../b`, `a//b`,
   `.hidden/x`, `/abs`, `a/b/c`, empty, NUL-embedded (via (buf,len)
   byte-scan), >129-byte refs, >64-byte components; accepts `acme/foo`,
   `foo`, `a-b.c_d/e.f`. Conflict rule: org-vs-flat 409 both directions;
   same-key-different-remote 409 (sidecar-based, no kb rows needed);
   credential-bearing clone URL → sidecar and all responses show the
   credential-free form only.
7. Purge-matrix fixture: scan + ingest + curator-drain a fixture project,
   assert every store in the matrix goes nonzero → zero via
   `purge-project`; the route reports per-store counts.
