# Implementation plan: Central agent-memory interception

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Companion to `central-agent-memory-interception.md` (design, converged R1–R4). This is the
**build sequence**: concrete files, signatures, shared contracts, and tests per slice. Four
slices, each its own branch + roundtable code review + PR + merge to `testing`, in order.
Conventions: **no `Co-Authored-By` / no Claude attribution** (branch-policy gate); commits
`type(scope): subject`; base each slice on the freshly-merged `origin/testing`.
Incorporates plan-review **R1** (30 items, 6 blocking; see end).

## Codebase grounding (verified)

- **DB1 schema** = `src/db1/schema.sql` (one CREATE-IF-NOT-EXISTS per line, TEXT ISO
  timestamps `DEFAULT (datetime('now'))`), embedded into `src/schema_data.h` as
  `AIMEE_DB1_SCHEMA_SQL` by a CMake custom command (`CMakeLists.txt:1357`). **It is exec'd
  in full at every DB1 open** by `db1_apply_schema_sqlite` (`src/db1/db_schema.c:52,66`), so a
  new `CREATE TABLE IF NOT EXISTS` **auto-creates on existing deployments** at next open. No
  `user_version` bump needed (this is how every table shipped). `db1_reconcile_columns` adds
  drift *columns*; tables are handled by the schema apply. `scripts/check-schema-sync.py` is a
  CI gate → regenerate + commit `schema_data.h`.
- **Accessor style:** `src/db1/wm.c`/`wm.h`. `db1_conn()`, sqlite3 prepare/bind/step/
  finalize, `now_utc()`. **SHA-256:** OpenSSL `SHA256()` + `SHA256_DIGEST_LENGTH` already used
  in-tree (`src/modules/db2/c/entity_nodes.c:43`); reuse directly.
- **Op + HTTP routes:** op handlers in `src/server/server.c` table (`{"wm.set", …}`); routes
  in `src/server/server_http_routes.inc` (`/v1/…`→op→handler, a `CAP_*`, `rh_dispatch_op`).
- **Hook seam:** `aimee hooks pre`→`src/cmd_hooks.c`→`pre_tool_check`
  (`src/guardrails_orchestrator.c:1196`); `guardrails_canonical_tool_name`. Cross-client via
  `configure-hooks.sh`. **Session-start:** `src/cli_session_start.c`. **Unit tests:**
  `aimee_add_test(name src.c …)` in `src/tests/CMakeLists.txt`. **CLI:** `src/cli_main.c` +
  `cmd_*.c` (model `src/cmd_memory.c`).

Timestamp domain: the design doc wrote `INTEGER`; the plan uses DB1's **TEXT-ISO**
`datetime('now')` to match every neighbour (design doc carries an R5 erratum noting this).

## Shared contracts: **P1 lands ALL of these as common library code, merged first**

Ownership (R2 #1/#5/#7/#9/#10/#11): every shared primitive is introduced by **P1** as pure,
fully-unit-tested library code with **no consumers yet**, so P2/P3/P4 only *use* them and
there is no cross-slice drift or orphaned helper. New files in P1:
`src/harness_memory_common.{c,h}` (hash + project resolver + caps constants),
`src/harness_memory_scope.{c,h}` (the scope registry), `src/harness_spill.{c,h}` (spill
producer **and** consumer + round-trip fixture), `src/harness_memory_audit.{c,h}` (audit log).

1. **`content_hash` domain (R1 #1; R2 #2/#6).** `hmem_content_hash(const hmem_row_t*)` (P1,
   the ONLY producer; P2 codec + P4 reconcile call it) = `sha256_hex( join("\x1f",
   [type, name, description, body, meta_json_canonical]) )`, each field **length-prefixed**
   (`"%zu:%s"`). Field normalization is explicit: NULL `description`→`""`; `meta_json`
   canonicalized to JSON with **sorted keys**, `null`/empty values **omitted**, `{}` when
   empty. Pinned by P1 tests incl. formatting-only body diffs, empty body, NULL description,
   missing optional keys.
2. **`project_id` vs `resolved_project_root`, two distinct values (R1 #3/#11; R2 #8).**
   - `resolved_project_root` = canonical FS root used for **all filesystem ops** (path
     validation, rematerialize, hydrate, spill metadata): `git -C cwd rev-parse
     --show-toplevel` else `realpath(cwd)`. Must be a real absolute path or **hard refuse**.
   - `project_id` = stable **DB key + log correlation** (the `project` column, ≤256): if
     `$AIMEE_PROJECT_ID` is set it is the id (may be opaque) **and** is validated to be an
     ancestor of `realpath(cwd)` or startup refuses; else `project_id = resolved_project_root`.
   `$AIMEE_PROJECT_ID` never substitutes for `resolved_project_root` in FS ops. Sharing is
   **per-worktree**. Resolver `hmem_resolve_project(project_id_out, root_out)` lives in
   `harness_memory_common` (P1) with tests for env-override, opaque-id, ancestor-rejection,
   worktree split, and terminal-refuse.
3. **Spill envelope (R1 #2/#18; R2 #2/#13).** `<AIMEE_HOME>/harness_spill/<project_hash>/
   <seq>-<pid>-<ctr>.json`. One JSON object: `{op:"upsert"|"tombstone"|"fail_open_allow",
   schema_version, project, name, type, description, body, meta_json, content_hash, op_id,
   ts}`. **`op_id = sha256(project "\x1f" name "\x1f" content_hash "\x1f" ts "\x1f" seq)`**
   (monotonic per-process `seq`), idempotent apply, collision-tested across concurrent
   producers. `fail_open_allow` records the agent's intended content when the hook had to
   ALLOW a local write (server-down): reconcile applies it **authoritatively** so a fail-open
   disk edit is **not** reverted by DB-wins (R2 #13). fsync file+dir; consumed→deleted;
   partial→log+skip; max-age cap. Producer+consumer+round-trip fixture all in `harness_spill`
   (P1). **Orphan-spill sweep (R2 #1):** session-start quarantines-and-audits spills older
   than the max-age, so spills can't accumulate across the P3→P4 window; a test asserts it.
4. **Capabilities (R1 #4/#15; R2 #11).** `CAP_MEMORY_READ` (get/list/search/render*),
   `CAP_MEMORY_WRITE` (upsert/tombstone/bulk_tombstone), constants defined in
   `harness_memory_common` (P1). Routes loopback-only (existing bind). Every mutating route
   rejects an unauthorized request (tested).
5. **Audit log (R1 #14; R2 #10).** `harness_memory_audit` (P1): `<AIMEE_HOME>/logs/
   interception.jsonl`, `0600`, size-rotated. A startup check resolves the path to a realpath
   and **refuses to start if it falls under any registered memdir** (self-redirect guard);
   the bulk-delete detector excludes the audit dir and `.git`.
6. **Positive scope (R1 #24; R2 #9).** `harness_memory_scope` (P1) is the **single** registry
   consumed by P3 detection and P4 hydrate: default `claude` → glob `**/memory/` matching
   `*.md` + `MEMORY.md`. NOT `AGENTS.md`/`CLAUDE.md`/`.cursorrules`/copilot-instructions. CI
   asserts P3 and P4 resolve the same set (one source, not two registries).
7. **Bounded fail-open (R2 #4).** Fail-open (server-down/spill-fail→ALLOW) stays the liveness
   default but is **bounded**: `harness_memory_common` tracks a per-session fail-open ALLOW
   counter; past `harness_memory.fail_open_max` (default 25) it **auto-flips to fail-closed**
   (deny further memory writes) and audits it; session-start **refuses/​warns loudly** if the
   prior session's fail-open count exceeded the threshold. Operator opt-in
   `harness_memory.fail_closed=true` makes it fail-closed from the start. Tested with injected
   server-down storms.

---

## P1: Foundations (shared library) + DB1 `harness_memory` store (PR: `feat/harness-memory-p1`)

P1 lands **both** the shared-contract library (above) **and** the DB1 store, all as
library code with no production consumers yet, so it merges first and the later slices only
consume. Bigger PR, but entirely pure functions + unit tests, easy to review.

**Foundation files (R2 #1/#5/#7/#9/#10/#11):** `src/harness_memory_common.{c,h}`
(`hmem_content_hash`, `hmem_resolve_project`, `CAP_MEMORY_READ/WRITE`, fail-open counter),
`src/harness_memory_scope.{c,h}` (registry + globber), `src/harness_spill.{c,h}` (write/
consume/sweep + round-trip fixture), `src/harness_memory_audit.{c,h}` (jsonl + rotation +
under-memdir refuse). Each with its own `aimee_add_test`.

**Store files:** `src/db1/schema.sql` (+regen `src/schema_data.h`), new
`src/db1/harness_memory.{c,h}`, `CMakeLists.txt` (sources → `aimee-core`),
`src/tests/test_harness_memory.c` (+`aimee_add_test`).

**Schema (one line):**
```sql
CREATE TABLE IF NOT EXISTS harness_memory ( id INTEGER PRIMARY KEY AUTOINCREMENT,
  project TEXT NOT NULL, name TEXT NOT NULL,
  type TEXT NOT NULL DEFAULT 'fact' CHECK (type IN ('fact','index','note','scratch')),
  description TEXT, body TEXT NOT NULL DEFAULT '', meta_json TEXT NOT NULL DEFAULT '{}',
  content_hash TEXT NOT NULL DEFAULT '', last_client TEXT NOT NULL DEFAULT '',
  source_session TEXT NOT NULL DEFAULT '', schema_version INTEGER NOT NULL DEFAULT 1,
  deleted_at TEXT DEFAULT NULL, created_at TEXT NOT NULL DEFAULT (datetime('now')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now')), UNIQUE(project, name));
CREATE INDEX IF NOT EXISTS idx_harness_memory_project ON harness_memory(project, deleted_at);
```
`type` CHECK is enforced **in SQL AND** re-validated in the accessor (R1 #10; CI asserts the
two type sets match).

**Accessors (`harness_memory.h`):** `hmem_row_t` (heap `description/body/meta_json`);
`hmem_content_hash` (the shared-contract #1 helper); `hmem_upsert` (`INSERT … ON
CONFLICT(project,name) DO UPDATE` setting all fields + `updated_at`, **clearing `deleted_at`
= resurrection**; no-op if live row's `content_hash` unchanged); `hmem_get`/`hmem_list`
(live-only unless `include_deleted`)/`hmem_search`/`hmem_tombstone`/`hmem_tombstone_prefix`
(bulk, ONE txn; R1 #4)/`hmem_rows_free`.

**Tests:** upsert→get; same-hash upsert no-op (updated_at unchanged); **hash domain pinned**
incl formatting-only body diff (R1 #1); list live-only vs `include_deleted`; **resurrection**:
upsert onto tombstoned clears `deleted_at` (R1 #12); explicit tombstone preserved; nested
`name` `topics/auth` round-trips; `_tombstone_prefix` all-or-nothing leaves siblings; **new
table auto-creates on a pre-existing DB** (open an old-schema `:memory:`/temp db, apply,
assert table present. R1 #5); SQL CHECK rejects a bad `type`.

---

## P2: server routes + codec + CLI (PR: `feat/harness-memory-p2`)

**Files:** `src/server/harness_memory_routes.c` (ops) + `server.c`/`server_http_routes.inc`
registration; `src/harness_memory_codec.{c,h}` (`encode`/`decode`, not "render"; R1 #23);
`src/server/harness_memory_store.{c,h}` (the serialized upsert+rematerialize writer + per-key
lock); `src/cmd_harness_memory.c` + `cli_main.c`; tests for codec, routes, concurrency.

**Ops/routes** (`rh_dispatch_op`): `harness_memory.{upsert,get,list,search,tombstone,
tombstone_prefix,render,render_index}` → `/v1/harness_memory/{…}` with the contract-#4 caps.
The request carries `client` (from `AIMEE_HOOK_CLIENT`, defaulting `claude`); unknown client →
identity codec + a warn logged **at the route layer** (R1 #30).

**Serialized write (R1 #4/#7/#8/#16).** `harness_memory_store_upsert(project,name,client,raw)`:
acquire the **per-`(project,name)` lock** from a map guarded by its own map-mutex (lazy
insert); the per-key lock is an **rwlock** (reads don't block on same-key writes). Under the
write lock: `decode`→canonical row, **server-side path validation** (realpath-chase, name
component check, `realpath(dirname) ⊂ realpath(memdir)`, same checks as P3, R1 #16),
`hmem_upsert`, then rematerialize. This is **serialized, not transactional** (R1 #7): the DB
commit can succeed and the file write fail → P4 reconcile recovers the DB-without-file case.
**Single-process model** is an invariant (one aimee-server owns DB1 + the file dir); a startup
assert documents it (R1 #8). Rematerialize: `mkstemp` **in `dirname(target)`** → `fsync` →
`rename`, always intra-directory so EXDEV cannot occur; the EXDEV branch is replaced with an
**assert + audit** (R1 #13).

**Codec:** `hmem_decode(client, raw_file, hmem_row_t*)` / `hmem_encode(client, row)→char*` /
`hmem_encode_index(client, rows, n)→char*`. v1 `claude` (YAML frontmatter + body; MEMORY.md
bullets). Round-trip audit helper used in tests; canonicalization is **not byte-exact**.

**CLI:** `aimee harness-memory {upsert,get,list,search,tombstone,render-index,reconcile}`
(reconcile stub → wired P4).

**Tests:** each op round-trips in-proc; codec stable for `claude`; index render from N rows;
**concurrency integration: N≥4 threads upsert same `(project,name)` via the real route → final
DB row and file agree, no torn file** (R1 #4); unauthorized mutating request rejected per route
(R1 #15); server-side path validation rejects `../`/abs/symlink-escape (R1 #16); EXDEV assert
path (fault-injected) signals + audits.

---

## P3: `memory_redirect` module + hook dispatch (PR: `feat/harness-memory-p3`)

**Files:** `src/memory_redirect.{c,h}`; one dispatch call in `pre_tool_check` AFTER existing
guardrail stages; `src/harness_spill.{c,h}` (contract #3); registry default + parse in
`config/` + `src/config_*`; audit helper (contract #5); `src/tests/test_memory_redirect.c`.

**Entry:** `memory_redirect_check(client, canonical_tool, tool_input_json, char**deny_json)`
→ `MR_ALLOW | MR_DENY`.

**Detection (contract #6):** edit-tool path under a memdir; registered memory tool; Bash
write-redirect target under a memdir (best-effort); per-file delete; **bulk/dir delete,
concrete heuristic (R1 #21):** an `rm -rf`/`rsync --delete`/`mv` whose resolved target dir
**is** (or contains) a registered memdir, OR ≥ `N`(=3) memdir-file deletions in one command;
**excludes `.git` internals and the audit dir**; negative fixture: unrelated `rm -rf
node_modules`/`git clean -fd` does not trip it.

**Path safety (R1 #6).** realpath-chase symlinks; **bind the resolved realpath** for both the
`⊂ memdir` assertion and the dispatch; the final-component file open uses **`O_NOFOLLOW`**
(openat-relative where available). Negative test: symlink swap between chase and open.
Hardlinks/bind-mounts/case-aliases documented out-of-scope for v1 (R1 #17) with tests for
parent-symlink escape.

**Verb boundary, enumerated (R2 #3):**
| operation | verdict | effect |
|---|---|---|
| Write/Edit to an in-scope `memory/*.md` | **redirect-deny** | server upsert + rematerialize |
| per-file delete of in-scope `*.md` | **redirect-deny** | server tombstone + remove file |
| bulk/dir delete hitting memdir | **redirect-deny** | `_tombstone_prefix` + rematerialize survivors |
| Write/Edit/delete of `MEMORY.md` | **reject-deny** | none (index is server-rendered) |
| anything failing path-safety / unsupported client / invalid name | **reject-deny** | none |
Redirect = server performs the op (loop-bypass holds, R1 #4); reject = no state change, no
tombstone. Negative tests assert each branch.

**Bounded fail-open (R1 #9; R2 #4):** server-unreachable → spill (`fail_open_allow` op,
carrying the agent's content so reconcile won't revert it) + ALLOW; spill-write-fail → ALLOW
+ **loud channel** (stderr banner + audit + `<AIMEE_HOME>/harness_spill/DEGRADED` marker). The
per-session fail-open counter (contract #7) **auto-flips to fail-closed** past
`fail_open_max`; `harness_memory.fail_closed=true` forces deny-on-failure from the start.

**Per-client deny:** `cmd_hooks.c` emits `*deny_json` per `AIMEE_HOOK_CLIENT` (Claude =
exit-0 stdout `permissionDecision:deny`); clients without a deny-with-message protocol →
out-of-scope + warn. **Loop bypass** structural (server/CLI rematerialize via `fopen`, never a
tool); `test_memory_redirect_no_loop` asserts it.

**Tests:** detection across `claude`+1 other; write→redirect-deny + upsert called; MEMORY.md
write/delete→reject-deny + NO tombstone; per-file delete→tombstone; bulk heuristic pos/neg;
symlink-escape + symlink-swap-TOCTOU rejected; **fault-injected** server-down→spill+ALLOW and
spill-fail→loud+ALLOW (R1 #22); spill round-trip fixture (with P4, contract #3); no-loop.

---

## P4: session-start reconcile + configure-hooks (PR: `feat/harness-memory-p4`)

**Files:** `src/cli_session_start.c` (+ reconcile routine, shared with `cmd_harness_memory.c`
`reconcile`), `configure-hooks.sh`/`.ps1` (registry defaults from contract #6, hook-before-
reconcile ordering, **hook-presence check**), `src/harness_spill.{c,h}` consume side, e2e test.

**Sequence (design §5; R1 #19; R2 #13):** acquire an **exclusive flock** on
`<AIMEE_HOME>/state/locks/<project_hash>.lock` for the whole reconcile+hydrate (R2 #14;
concurrent session-starts serialize, external mid-hydrate edits can't tear an import) → verify
hook installed → **sweep orphan spills** (contract #3) → **consume spills FIRST, authoritative**
(`upsert`/`tombstone`/`fail_open_allow` applied as the truth, updating content_hash/updated_at,
idempotent by `op_id`) so offline + fail-open disk edits are **not** reverted (R1 #19, R2 #13)
→ deterministic reconcile by `content_hash`: DB1-only→rematerialize; disk-only→import;
mismatch→**DB1-wins** + log divergence; tombstoned→remove file; equal→noop → **incremental
hydrate** keyed on per-project marker + per-file (mtime,size) cache (R1 #20); full pass first
run. Memdir created `0700` if absent.

**`import-only` mode (R2 #12)**, entered when the hook is missing (interception is OFF, so we
must not fight the user's disk): a **read-only disk scan that imports to DB1 only**,
**rematerialize and DB1-wins are DISABLED** (no overwriting disk the missing hook can't
guard); reconcile is restricted to disk→DB import. **Session-scoped**, with a persistent
audit warning until the hook is restored. Test asserts no rematerialize occurs while the hook
is absent.

**Manual trigger / revert path (R1 #28):** `aimee harness-memory reconcile` re-runs the routine
mid-session and is the documented recovery if P4 is reverted (re-import from disk).

**Tests:** spill-before-mismatch authority (DB older, disk/spill newer → not reverted, R1 #19);
duplicate/partial spill idempotency; each reconcile branch; hook-absent→import-only+warn;
incremental marker skips unchanged files; project_id terminal-refuse + per-worktree split
(R1 #3/#11). **e2e runs on EVERY PR in a throwaway `$HOME`/tmpdir + chroot-style sandbox**
(no PVE needed for core cases; R1 #26): agent Write to `memory/x.md`→redirect-deny→DB1+file;
second session sees it; `rm -rf memory/`→tombstones; hook removed→import-only. A **PVE CT** is
used only for a fuller multi-client manual pass (nightly/manual label), **torn down after**.

---

## Cross-cutting

- **Bisectability (R1 #28/#29):** P2's route is dormant-but-fully-tested until P3; the
  **P3→P4 window is functional-but-unreconciled** (redirection active, self-healing arrives
  with P4's session-start), flagged for reviewers. Each slice keeps `testing` green.
- Roundtable the **actual diff** before each PR; fold real findings; post a review-summary PR
  comment; commit fixes as `fix(scope): … (review)`.
- **Cleanup:** remove the `aimee-hmem` worktree + any PVE CT at the end.

## Plan review revisions (R1)

Clean .254 roundtable (6/6 panelists, `degraded:false`), 30 items, 6 blocking, slice
structure validated, all items were "pin the spec." Folded: hash domain (#1), spill envelope
+ idempotency (#2/#18), project_id terminal+semantics (#3/#11), P2 harden-don't-reslice +
concurrency test + caps + single-process model (#4/#8/#15), new-table auto-create note (#5),
TOCTOU/O_NOFOLLOW (#6), SQL CHECK mirror (#10), serialized-not-atomic (#7), fail-open
threat-model + loud channel + fail_closed opt-in (#9), tombstone resurrection fixture (#12),
EXDEV→assert (#13), audit-not-under-memdir (#14), server-side path validation (#16), hardlink
scope (#17), spill-authoritative-before-DB-wins (#19), incremental change detector (#20),
bulk-delete heuristic (#21), per-slice test matrix + fault injection (#22/#25), positive scope
single-source (#24), per-PR non-PVE e2e (#26), codec encode/decode rename (#23), client-id
source (#30), design-doc timestamp erratum (#27), revert + P3→P4-window notes (#28/#29).

## Plan review revisions (R2 / convergence): APPROVED

Clean .254 round (5/6, `degraded:false`), 14 "blocking", but **6 were one root cause**: the
shared primitives had no owning slice. Resolved structurally, **P1 is now foundations-first**,
landing `harness_memory_common` (hash + project resolver + caps + fail-open counter),
`harness_memory_scope`, `harness_spill` (incl. consumer + orphan sweep) and
`harness_memory_audit` as library code merged before any consumer (R2 #1/#5/#7/#9/#10/#11).
Distinct items folded: `project_id` vs `resolved_project_root` split (#8); `op_id` derivation +
hash NULL/empty normalization (#2/#6); bounded fail-open, rate-limit→auto-fail-closed +
startup refusal (#4); spill `fail_open_allow` op so reconcile won't revert fail-open disk edits
(#13); precise `import-only` mode (#12); hydrate flock (#14); enumerated redirect/reject table
(#3).

**Decision: plan APPROVED.** Two clean plan rounds produced only ownership/edge-spec
tightenings (now folded), no architectural objection to the slicing. Further rounds yield
diminishing nitpicks better caught in each slice's **own** code-level roundtable (the real
diff) before its PR. Proceeding to PR0 (docs) → P1.
