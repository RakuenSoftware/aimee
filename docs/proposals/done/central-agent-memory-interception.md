# Proposal: Central agent-memory interception — redirect any agent's local memory into the aimee-server store

- **State:** done
- **Completed:** 2026-06-28
- **Moved from:** `docs/proposals/pending/central-agent-memory-interception.md`
- **Summary:** **all four phases shipped to `testing`, live-validated on a dev split
  server** (a dev deployment, not production). P1–P4 (DB1 store + routes/CLI + interception + session-start hydration) plus
  the v1.1/v1.2 follow-ups (multi-client scope registry, spill durability + audit log,
  Bash-write interception, full reconcile, config-file scope override, remote-server
  project key, real-time inotify backstop) and two e2e-found fixes (#817 startup segfault,
  #819 split-server wiring). The approved test plan was executed (9/9 unit suites + a full
  dev split-server e2e) and surfaced one further fix (#829 per-client scoping on a
  shared server). **"Done" = the v1 feature is built, merged, and dev-validated — it does
  NOT mean production-ready/GA or safe for untrusted/remote/multi-tenant use**; the
  security-relevant GA gates (prompt-injection provenance, RM3/4/8 remote auth, the
  cross-client trust boundary, the non-Linux platform window) are recorded in
  "Implementation status → Close-out scope". Design history: roundtable reviews R1 (design, 28 items),
  R2 (degraded), R3/R4 (clean) — see the "Review revisions" sections.
- **Thesis:** An agent's *local, self-owned* memory (today: the Claude Code harness
  file-memory at `~/.claude/projects/<proj>/memory/*.md` + `MEMORY.md`, written via the
  `Write`/`Edit` tools) should not be owned by the agent. aimee already hooks every
  agent's tool calls cross-client; it should **intercept memory writes, persist them
  centrally in the aimee-server database (DB1), and own the on-disk projection** so that
  memory is shared per-project across all agents/sessions on the host, curatable, and
  never silently owned by a single ephemeral agent.

## Goal

Intercept **any** agent writing to its own local memory and redirect the write to a
single central store, **without** the agent being able to keep a private copy:

1. **Capture** — agent memory writes are detected at the existing cross-client hook seam
   and persisted to the aimee-server store.
2. **Block** — the agent's own write is denied; the agent never owns the bytes on disk.
3. **Re-materialize** — aimee (not the agent) writes the authoritative file back, so
   native reads (`Read`/`Glob`/`Grep`) and the harness's automatic recall keep working.
4. **Share** — memory is keyed per-project and shared across every agent/session/client
   working that project on the host.

Destination is **DB1** — aimee-server's own SQLite database
(`~/.config/aimee/aimee.db`, `src/db1/`), opened by the server at
`src/server/server.c:1616` via `db1_init` (`src/db1/db1_init.c:54`). **Explicitly not**
the kb's db2/Postgres, where the existing `memory.store` RPC lands
(`src/kb/kb_service_memory.c:1418` → `src/modules/db2/c/kb_service_backend_memory.c:1171`). This is
a new, parallel store; the kb memory path is untouched.

## §0 What already exists (so we don't rebuild it)

- **Cross-client hook seam — LIVE.** `configure-hooks.sh` installs a `PreToolUse` hook on
  `Edit|Write|MultiEdit|Bash|Read|Glob|Grep|Task` → `aimee hooks pre` for Claude Code,
  Gemini CLI, Codex CLI, and GitHub Copilot, each with `AIMEE_HOOK_CLIENT=<client>`.
  Handler: `src/cmd_hooks.c` (`cmd_hooks`, line 116) → `pre_tool_check`
  (`src/guardrails_orchestrator.c:1196`).
- **Tool-name normalization — LIVE.** `guardrails_canonical_tool_name`
  (`src/guardrails_orchestrator.c:121`) maps per-client tool names to one vocabulary
  (`write_file`→`Write`, etc.), so detection logic is written once.
- **Block + feedback mechanism — LIVE.** The pre handler can deny a tool (exit 2 /
  PreToolUse JSON, `src/cmd_hooks.c:402`) and feed text back to the model — the same
  mechanism `scripts/hooks/redirect_grep.py` uses to redirect `grep`.
- **DB1 store — LIVE.** Server-owned SQLite with a per-session key/value table
  `working_memory` (`src/db1/wm.c`, `src/db1/schema.sql`). New accessors model on it.
- **Session-start hook — LIVE.** `aimee session-start` (`src/cli_session_start.c`) already
  runs at startup/resume/compact and POSTs to the server; the natural place to hydrate.
- aimee does **not** today reference the Claude memory dir or `MEMORY.md` anywhere (the
  only `~/.claude/projects` touchpoint is reading transcripts at `src/config_save.c:1192`).

## §1 Source of truth: DB1 is authoritative; the file is an aimee-owned projection

The single most important invariant (R1 #1). DB1 holds the canonical memory. The on-disk
`memory/*.md` files are a rendered cache that aimee rebuilds — DB1 wins on any conflict.

**Threat model — be honest about what is and isn't intercepted (R3 #2).** We do **not**
claim "every mutation path is intercepted." We intercept exactly **agent-tool-mediated**
writes/deletes (the seam fires only on tool calls — §0). Writes that bypass the tool layer
entirely — a human in `vim`, IDE autosave, a sync client (Dropbox/Syncthing), a container
volume mount, `python -c 'open(...).write()'` via `Bash`, or an agent **not** installed
with an `AIMEE_HOOK_CLIENT` hook — are **not** captured at write time. They are caught by a
**session-start content-hash audit** (§5) that compares every on-disk file against its DB1
row and, per the deterministic reconcile policy, makes disk match DB1 (or imports
disk-only files). So the precise invariant is:

- **Agent-tool writes** → intercepted (§3): denied, canonicalized into DB1, re-materialized.
- **Agent-tool deletes** → intercepted (§3): tombstoned in DB1 (per-file *and* bulk/dir, §3).
- **Non-tool / external changes** → **not prevented**, reconciled to DB1 at session-start
  (§5) with the divergence surfaced in the audit log (§7). DB1 is authoritative; an external
  edit does not silently become the source of truth.
- aimee's **own** re-materialize writes use direct filesystem syscalls in the server/CLI
  process and therefore **cannot** re-enter a PreToolUse hook (hooks fire only on *agent
  tool calls*, not on aimee's own process I/O). This makes the no-loop property
  **structural**, not flag-based (R1 #2, #23); a test exercises aimee's writer with the
  hook installed to prove it.

Accepted scope: **single host.** DB1 is local SQLite, so "shared across all agents/
sessions" means *on this machine*. Cross-machine sharing (CI, remote boxes, teammates) is
a **non-goal** here and a documented future path (sync DB1→db2, or write-through to db2),
chosen deliberately per the "server DB, not kb DB" requirement (R1 #10).

## §2 The store (DB1 schema + accessors)

New table in `src/db1/schema.sql`, accessors in a new `src/db1/harness_memory.c`
(modeled on `src/db1/wm.c`), registered in `db1_run_migrations`
(`src/db1/db_schema.c:20`):

```sql
CREATE TABLE IF NOT EXISTS harness_memory (
  id            INTEGER PRIMARY KEY,
  project       TEXT NOT NULL,        -- canonical project id (normalized worktree root; §6)
  name          TEXT NOT NULL,        -- relpath under the memory dir, NO extension (e.g.
                                      -- 'topics/auth'); each '/'-separated component matches
                                      -- ^[A-Za-z0-9._-]{1,64}$, no '..', no leading/trailing '.'
  type          TEXT NOT NULL DEFAULT 'fact'
                  CHECK (type IN ('fact','index','note','scratch')),
  description   TEXT,                 -- nullable; canonical `description:` frontmatter
  body          TEXT NOT NULL,        -- CANONICAL client-neutral markdown body (NOT raw bytes)
  meta_json     TEXT,                 -- canonical extra frontmatter (tags, etc.) as JSON
  content_hash  TEXT NOT NULL,        -- SHA-256 of (type|name|description|body|meta_json);
                                      -- idempotency (R1 #26) + reconcile compare (R3 #13)
  last_client   TEXT,                 -- last writer (AIMEE_HOOK_CLIENT); provenance, not a key
  source_session TEXT,                -- provenance only, not an isolation key
  schema_version INTEGER NOT NULL DEFAULT 1,
  deleted_at    INTEGER,              -- tombstone (R1 #8, #12); NULL = live
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL,
  UNIQUE(project, name)
);
```

Accessors: `harness_memory_upsert`, `_get`, `_list(project)`, `_search(project, query)`,
`_tombstone(project, name)`, `_tombstone_prefix(project, dir)` (bulk/dir delete — §3),
`_render(project, name, client)`, `_render_index(project, client)`. By default `_list`/
`_get`/`_search` return only live rows (`deleted_at IS NULL`); tombstoned rows require an
explicit `include_deleted` flag (R3 #24).

- **Canonical, not raw (R3 #1).** One shared row per `(project,name)` IS the canonical
  memory; `body`/`description`/`meta_json` are a **client-neutral** normalization, not one
  client's raw bytes. On capture, each client's frontmatter+body is parsed *into* these
  canonical fields; on hydrate they are *rendered back* into the target client's format
  (§4). This resolves the earlier contradiction (raw per-client bytes under a single
  `(project,name)` key would let two clients silently clobber each other, and hydrating
  client A's bytes to client B was wrong). `last_client` is provenance only. **Trade-off:**
  canonicalization is deliberately *not* byte-exact round-trip — a client re-reading its own
  write sees the canonically-rendered, semantically-equivalent form (R3 #21, see Risks).
- **Nested names (R3 #15).** `name` is the relative path under the memory dir, so
  `memory/topics/auth.md` ↔ `name='topics/auth'`. Slashes are allowed *between* validated
  components; the per-component charset still forbids `..` and leading/trailing dots. (Dots
  *within* a component — `foo.bar` — are allowed; only `..` and edge dots are rejected,
  resolving the earlier regex-vs-prose ambiguity.)
- **`content_hash` = SHA-256** over the canonical tuple (R3 #13); hook and server share one
  implementation. Used for retry-after-deny idempotency *and* as the reconcile compare key.
- **Tombstone semantics** (R2 #6): `_tombstone` sets `deleted_at` on the live
  `(project,name)` row; hydration removes the file. A later write to the same
  `(project,name)` **intentionally** revives it (clears `deleted_at`, replaces content).
  `content_hash` is not the delete/resurrection key — the `UNIQUE(project,name)` row is — so
  a same-name re-create is a normal revive.
- **Encoding** (R2 #8): bodies are UTF-8 text; non-UTF-8 / binary payloads are rejected at
  capture (memory files are markdown).
- **Migration** (R2 #5): `schema_version` bumps only on a breaking change; migrations are
  additive (new columns, backward-compatible defaults) via `db1_run_migrations`, so older
  rows read under current accessors and a rolling upgrade can't strand data. DB1 runs in WAL
  mode; the server holds no cross-request in-memory cache of rows/hashes (it reads DB1 per
  request), so there is no stale-cache hazard after a restart (R3 #11).

## §3 Interception (a dedicated module, not tangled into guardrails)

New module `src/memory_redirect.c/.h`, called from `pre_tool_check` via a single
dispatch point so guardrail logic and memory logic stay separate (R1 #18):

```
memory_redirect_check(client, canonical_tool, tool_input) -> {ALLOW | DENY(reason)}
```

**Detection** uses a config-driven **memory-surface registry** (§6): a tool call is a
memory mutation if it is

- an edit tool (`Write`/`Edit`/`MultiEdit`) whose target path resolves under a registered
  memory dir for `client`; **or**
- a registered **memory tool** name (for agents that store memory via a tool, not a file);
  **or**
- a `Bash` command whose write target (`>`, `>>`, `tee`, `sed -i`, heredoc) resolves under
  a registered memory dir (best-effort; process substitution, subshells, eval-indirection
  and symlink targets can evade it — see Risks for the honest limit) (R1 #3, R2 #4); **or**
- a **per-file delete** (`Bash` `rm`/`unlink`, or a client delete tool) targeting a
  registered memory file → tombstone that row; **or**
- a **bulk/directory destructive op** (`rm -rf <memorydir>`, `mv`/`rsync --delete`/
  `cp --remove-destination` whose source or target resolves to a registered memory dir):
  parse the directory argument(s), `realpath`-confine under a registered memory dir, then
  `_tombstone_prefix(project, dir)` to bulk-tombstone every matching row in one transaction,
  DENY the command, and re-materialize the surviving cache (R3 #5). This is a distinct code
  path from per-file delete and has its own test (`rm -rf memory/` → all rows tombstoned).

**Write flow** (R1 #4, #7):
1. Parse the client's frontmatter + body and **canonicalize** into `type`/`name`/
   `description`/`body`/`meta_json` (§2). Fallback when frontmatter is absent: `name` from
   the relpath, `type=fact`, `description` NULL (R1 #17).
2. **Resolve + validate the path (R2 #2, R3 #9).** `realpath`-resolve the target, *chasing
   all symlinks first*, then assert `realpath(dirname(target))` is a prefix of
   `realpath(project_memory_dir)` — rejecting any path that escapes the memory dir via a
   symlink/bind-mount into `/etc`, a sibling project, or elsewhere (R4 #3). Validate each
   `name` component against `^[A-Za-z0-9._-]{1,64}$` and reject any `..`, leading/trailing
   `.`, abs path, or null byte. Enforced **both** in the hook and server-side.
3. **POST to the running server** `/v1/harness_memory/upsert` (new route). **The server is
   the sole DB1 writer — the hook NEVER writes DB1 directly** (R1 #4, #11; R2 #1). No
   direct-SQLite fallback races it. If `content_hash` matches the live row, it is a no-op
   (R1 #26).
4. **The server holds the per-`(project,name)` mutex across the WHOLE
   upsert→rematerialize sequence (R4 #2)** — not just the DB write — so the DB1 row and its
   on-disk file are updated atomically as a unit. Without this, two concurrent writes to the
   same name could interleave (A upserts, B upserts, A rematerializes) and leave disk
   showing A while DB1 holds B. Under the lock: upsert the canonical row, then re-materialize
   with `mkstemp` **in the target's own directory** (`dirname(target)`, so `rename(2)` is
   same-filesystem and atomic) → `fsync` → atomic `rename`, all **before** the hook returns.
   If the target dir is on a different filesystem (EXDEV would force a non-atomic copy), it
   **refuses to materialize and spills instead** (R3 #9/#22). Atomic same-fs `rename` means a
   concurrent reader sees the old or new file, never a partial one, and there's no empty-read
   window (R1 #7; R2 #3 — advisory locks unnecessary). A `_tombstone_prefix` bulk delete is
   likewise one all-or-nothing DB1 transaction, with every per-row outcome audited (R4 #4).
5. Hook returns a **deny-with-redirect** worded as success, e.g. *"Saved to aimee memory
   (id=N); the on-disk file now reflects your content — do not re-write it."* (R1 #14).

**Two deny kinds — disambiguated (R4 #6).** "Deny" is overloaded; the design uses two
distinct verdicts, and reconcile must respect the difference:
- **Redirect-deny** (normal write; per-file & bulk delete): deny the agent's *raw tool call*
  but **aimee performs the operation itself** — upsert/tombstone in DB1 + re-materialize. The
  intent IS effected, just through aimee, never the agent's raw bytes.
- **Reject-deny** (MEMORY.md write/delete; unsupported/invalid ops): deny **and change no
  state** — no upsert, no tombstone, no file mutation. A reject-deny must be incapable of
  causing a later reconcile to alter on-disk state. In particular, a denied delete of
  `MEMORY.md` produces **no tombstone**; the index is simply re-rendered.

**Deny / liveness contract (R3 #4) — one delivery mechanism, applied uniformly.** Both deny
kinds are delivered as a *deny-with-message that does not error the agent*. The mechanism is
per-client and must be specified, not assumed:
- **Claude Code:** exit 0 with stdout JSON
  `{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny",
  "permissionDecisionReason":"<msg>"}}`. (A *non-zero* exit is the harder "block with
  stderr" — we do **not** use it for the success/redirect path; the earlier "non-zero exit"
  wording was wrong.)
- **Gemini / Codex / Copilot:** use each client's documented deny-with-feedback equivalent.
  Any client **without** a deny-with-message hook protocol is **out of v1 scope** and listed
  as unsupported (R3 #4, #10) rather than silently mis-handled.

**Failure policy — liveness-wins, spill-always (R1 #6; R2 #1; R3 #3/#4/#8).** A successful
capture **always** writes a spill record too, so a server/DB crash between upsert and
materialize still reconciles. If the server is unreachable: write the spill, **ALLOW** the
original tool (the agent writes its own file this once — caught and canonicalized at the
next session-start audit), and log a `server-unreachable` interception event (§7). We never
deny-without-persisting (no silent drop) and never block the agent on our own outage.

**Spill contract (R3 #8):**
- **Location:** `<AIMEE_HOME>/harness_spill/<project_id>/<seq>-<rand>.json` (a stable
  filesystem under `AIMEE_HOME`, **not** `/tmp`). `<rand>` varies per spill (no `Date.now`/
  RNG dependency in hot path; derived from pid+counter).
- **Format:** a JSON envelope carrying exactly the fields a server upsert receives plus
  `schema_version` and `op` (`upsert`|`tombstone`).
- **Durability:** `fsync` the spill (and its dir) before the hook exits.
- **Retention:** consumed by session-start reconcile then deleted; a max-age cap prevents
  unbounded growth.
- **Recovery:** the reconcile parser treats a truncated/partial spill as opaque — logs it
  and does **not** auto-replay (documented as the one place data *can* be lost: a hook crash
  mid-spill-write). Everything else round-trips.
- **Spill-write failure (R4 #8):** if the spill itself can't be written/fsync'd (disk full,
  bad perms, corrupt `AIMEE_HOME`) this is the irreducible failure — emit a loud stderr
  error and the audit event (§7), still **ALLOW** the tool (no block), and do not pretend it
  persisted. This is the only "data may be lost" path and it is surfaced, never silent.

## §4 Re-materialization & MEMORY.md

- **Per-client rendering:** the server renders each canonical row (§2) back into the target
  client's frontmatter/body format and `_render_index` produces that client's index file.
- **MEMORY.md is a server-rendered index** (`type='index'`), regenerated from the project's
  live rows. An agent `Write`/`Edit` to `MEMORY.md` is **denied with a guiding message** —
  *not* silently discarded (which would cause an endless re-write loop). The deny reason
  tells the agent the correct affordance: *"MEMORY.md is auto-rendered from your memory
  entries; to add or change one, Write a file under `memory/<name>.md`."* (R3 #6). Discarded/
  denied MEMORY.md writes are logged (§7).
- Writes are atomic (`mkstemp` in the target dir → `fsync` → `rename`, §3 step 4) with a
  documented file mode.

## §5 Migration, hydration, reconciliation (session-start)

`aimee session-start` (`src/cli_session_start.c`), per project. The same routine is exposed
as a **manual `aimee harness-memory reconcile`** command so a user can force a sync mid-
session without waiting for the next session-start (R4 #1).

**Hook-independence + fail-loud (R4 #5).** Reconcile is **not** gated on the hook: it runs
at session-start regardless, so on-disk state is always pulled into DB1 even if no hook is
installed. Session-start additionally **verifies the memory hook is installed** and, if it
is missing or mis-wired, emits a loud warning to the user (and an audit event, §7) that
**at-write interception is OFF for this session — memory is import-only until the hook is
restored**. So a missing hook degrades to "session-start import" (no data loss), never to
silent escape. **Durability boundary:** only on-disk state present *at a reconcile* is
captured; an external edit made and then reverted entirely within one session, between
reconciles, is not observed (R4 #1) — documented, with the manual trigger as the mitigation.

1. **Consume spills** first (§3): replay each well-formed spill envelope through the normal
   server upsert/tombstone path, then delete it; partial spills are logged and skipped.
2. **Deterministic, bidirectional reconcile (R3 #3/#5).** Compute each on-disk file's
   canonical `content_hash` and compare to DB1, applying a single precedence rule:
   - **DB1 row present, no file** → re-materialize from DB1 (covers a `rm`/bulk-delete the
     hook missed; DB1 wins, so an un-intercepted delete is *restored*, not honored — the
     supported way to delete is the intercepted path, §3).
   - **File present, no DB1 row** → import it (one-time/orphan case, R1 #13/#27): canonicalize
     and upsert, then re-materialize canonical.
   - **Both present, hashes differ** → **DB1 wins**: overwrite the file from DB1 and log a
     `reconcile-divergence` event (§7) naming the file. (An external edit never silently
     becomes the source of truth.)
   - **Tombstoned in DB1** → ensure the file is absent.
   - **Both present, equal hash** → no-op.
3. **Hydrate** any remaining live rows → disk **incrementally** (rows whose `updated_at` >
   the per-project last-materialized marker), full pass on first run (R1 #19). Concurrent
   session-starts are last-writer-wins for identical DB1 state, which is idempotent.

## §6 The memory-surface registry (generality)

A config table (under `config/`, surfaced as an aimee config key, validated at
session-start) mapping `client` → `{ memory_dir_globs[], memory_tool_names[] }`. Default
entries ship for known clients (Claude Code's `memory/` dir today; others as identified).
New agents are added **declaratively, no code change** (R1 #20, #27). Clients with no
registered surface emit a startup warning rather than silently doing nothing (R1 #20).

**`project_id` derivation (R3 #14)** — deterministic precedence: (1) `$AIMEE_PROJECT_ID`
if set; else (2) the **enclosing git worktree root** via `git -C <cwd> rev-parse
--show-toplevel` (honoring the cwd) — each worktree has its own toplevel, so **worktrees do
NOT share memory** with the main checkout by default (intentional; they're different working
trees); else (3) `realpath(cwd)`. The result is hashed to a stable `project_id`. Symlinked/
case-only path differences are normalized via `realpath` before hashing. **Trust boundary
(R4 #9):** `$AIMEE_PROJECT_ID` is honored only as a local convenience — any process that can
set the env can address any project's memory, so it is **not** a security boundary; the
store is single-host/single-user (§1) and `project_id` is namespacing, not access control.
The **memory dir is created by aimee** (mode `0700`, owned by the user) if absent, so
`realpath` confinement always has a concrete root (R4 #14).

**Scope of "memory" (R3 #16)** — v1 captures **only** the registered per-client memory
surfaces (Claude's `memory/*.md` + `MEMORY.md`). Durable-context files that are *not* the
agent's scratch memory — `AGENTS.md`, `CLAUDE.md`, `.cursorrules`,
`.github/copilot-instructions.md` — are **explicitly out of v1 scope** (they're
human-authored project config, often version-controlled). Adding any of them is a one-line
registry entry later if desired.

## §7 Audit log & hook ordering

- **Interception audit log (R3 #12).** Every interception decision is appended as a
  structured JSON line to `<AIMEE_HOME>/logs/interception.jsonl` (rotated): `ts, project_id,
  client, tool, path, action (intercepted|denied|tombstoned|reconciled|spilled|imported|
  divergence|server-unreachable|missed), reason, content_hash`. This is the incident-response
  trail for a layer that DENYs tool calls, and the source for any "memory reconciled / X
  diverged" notices surfaced to the user.
- **Hook ordering (R3 #18).** `memory_redirect_check` runs as one stage within
  `pre_tool_check` (§3). Order: existing guardrails (TDD/worktree/blast-radius) run first;
  memory redirection runs after and, on a memory-surface match, **short-circuits with its
  deny-with-redirect** (no later stage can override an already-denied call). A non-memory
  tool passes straight through unchanged. This keeps guardrail semantics intact and makes
  the memory verdict explicit rather than racing other stages.

## Phasing (each independently shippable, roundtable-reviewed before its PR)

- **P1** — DB1 `harness_memory` table (canonical fields, SHA-256 hash, NOT NULL, tombstone)
  + accessors (incl. `_tombstone_prefix`, `_render*`) + migration (§2). Unit tests.
- **P2** — `/v1/harness_memory/*` server routes (upsert/get/list/search/tombstone/
  bulk-tombstone/render/render-index) with the per-`(project,name)` mutex + canonicalize↔
  render codec + `aimee harness-memory` CLI over the same handlers (§2–§4).
- **P3** — `src/memory_redirect.c` + dispatch stage in `pre_tool_check` (§7 ordering):
  detection registry, write + per-file + bulk/dir delete capture, symlink-safe path
  validation, the per-client deny-with-redirect contract, spill-always + liveness-wins
  failure path, and the interception audit log (§3, §7). Cross-client interception test +
  loop-bypass test.
- **P4** — session-start spill-consume → deterministic reconcile → incremental hydrate (§5)
  + `configure-hooks.sh` hook-before-reconcile ordering and default registry entries (§6).
  End-to-end test.

## Implementation status

**All four phases shipped to `testing`** (each its own PR, code-level roundtable-reviewed
before merge per the phasing contract), plus v1.1/v1.2 follow-ups and the e2e/test-plan
fixes. Modules: `src/db1/harness_memory.{c,h}`, `src/harness_memory_common.{c,h}`,
`src/server/harness_memory_routes.c`, `src/memory_redirect.{c,h}`,
`src/harness_memory_hydrate.{c,h}`, `src/harness_memory_watch.{c,h}`.

**Core phases:**
- **P1 (#794)** — DB1 `harness_memory` table + accessors + `harness_memory_common` (vendored
  SHA-256 `content_hash`, project resolver). Timestamps are TEXT-ISO `datetime('now')` /
  `deleted_at TEXT` per DB1 house style (the §2 sketch's `INTEGER` was an erratum, same
  semantics).
- **P2 (#795)** — `/v1/harness_memory/*` routes (upsert/get/list/search/tombstone/
  bulk-tombstone/render) + `aimee harness-memory` CLI over the same handlers; server is the
  **sole DB1 writer**.
- **P3 (#798)** — `memory_redirect` interception stage in `pre_tool_check`: detect a
  `Write`/`Edit` to a registered memory surface → **redirect-deny** → central store; aimee
  re-materializes the file via a direct syscall (structural loop-bypass). `MEMORY.md` →
  reject-deny with guidance.
- **P4 (#802)** — session-start hydration DB1→disk with name-slug + write-confinement under
  the project memdir.

**v1.1 (#804/#806/#808):** config-driven **multi-client scope registry** (`harness_memory_scope`;
adding an agent is one table row) + atomic hydrate; **spill durability** (`harness_memory_spill`
producer/consumer) + **audit log** (`harness_memory_audit`) + session-start replay; **Bash-write
interception** (reject-deny shell writes to memory files, quote-aware).

**v1.2 (#809/#811/#813/#815):** **full reconcile** (disk-only `import_orphans` + tombstone
removal, DB1-wins on hash mismatch); **config-file scope override**
(`AIMEE_HARNESS_MEMORY_SCOPES` / `<AIMEE_HOME>/harness_memory_scopes.conf`); **remote-server
project key** (thin client resolves + forwards `harness_project`, server validates);
**real-time inotify backstop** (`harness_memory_watch` + `aimee harness-memory-watch`;
Linux-only, no-op elsewhere) closing the at-write gap for non-tool edits.

**e2e + test-plan fixes:**
- **#817** — plugin-loader startup segfault (large `plugin_t` arrays were stack-allocated,
  overflowing the main-thread stack on a plain non-container deploy; CI's Docker masked it) →
  heap-allocated.
- **#819** — interception was never wired into the **split** server: `handle_hooks_pre` in
  `server/server.c` called the guardrails directly and never ran `memory_redirect`, and
  `memory_redirect.o` wasn't linked into `aimee-server`. Fixed: `server_memory_intercept()`
  runs before the guardrails, writes DB1 directly (`hmem_upsert`), mirrors to disk
  (`memory_redirect_rematerialize`, confined under `projects_root`); module linked into the
  server.
- **#829** (found by executing the approved test plan) — a shared split server mis-scoped
  every agent to one client because it read the server's own `AIMEE_HOOK_CLIENT`; the thin
  client now forwards `AIMEE_HOOK_CLIENT` as `harness_client` and the server reads it
  per-request (env fallback only for the local/combined path).

**Validation.** The approved test plan was executed: 9/9 unit suites pass; a dev
split-server e2e covered functional (F1–F10), reconcile (RC1–RC9), fail-open, concurrency,
security (traversal/symlink/cross-project), Bash TP/FP vectors, and the watcher — all PASS.

**Deferred — pre-GA hardening / validation (explicit future-work, not v1-correctness):**
- **Divergence audit counters** — `overwrite-divergent` / `removed_hash` audit metrics over
  the reconcile path (the reconcile *behavior* is shipped + tested; this is observability).
- **`AIMEE_FAULT` fault-injection seam** — a deterministic fault hook to exercise the
  spill/fail-open paths under test without a real outage.
- **Remote auth/replay hardening (RM3/RM4/RM8 — the test plan's remote-transport risk
  items: caller authentication, request integrity, and replay resistance for untrusted
  remote callers).** The remote-server project-key path ships; this hardening layer is the
  next step and is GA-blocking for remote exposure (see Close-out scope).
- **E-PROD deploy** — promotion + soak on a production deployment (validated on a dev split
  server; not yet GA-deployed).
- **Honest limits carried from Risks** — canonicalization is semantically-equivalent, not
  byte-exact (deliberate cost of one `(project,name)` key shared across clients; round-trip
  fidelity is audited + logged); shared memory bodies are untrusted (prompt-injection
  surface — size-capped, no auto-exec; deeper tagging/review is a follow-up).

### Close-out scope: what "done" does and does not mean (R5)

A close-out roundtable (6 panelists, 0 failed, not degraded) agreed the feature is shipped
and validated and that the proposal may be filed to `done/`, **conditioned on the done
record stating the accepted residual risk and GA gates explicitly** (the panel's blocking
item offered exactly this — a recorded threat model — as the alternative to building the
deferred mitigations first). "Done" here means **the v1 feature is built, merged, and
validated on a dev split server** — it does **not** mean production-ready, GA, or safe to
expose to untrusted/remote/multi-tenant callers. The following are recorded as
**GA-blocking gates**, reclassified from generic future-work:

- **Untrusted shared-memory / cross-agent prompt-injection (GA-blocking for multi-principal
  use).** One `(project,name)` row shared across clients + session-start hydration is, by
  design, a path for one agent (or an external edit) to plant body text later hydrated into
  another agent's context. **v1 trust model:** *all clients sharing a project are mutually
  trusted* (single-user host). Multi-principal / multi-tenant use is **not supported** until
  provenance/taint metadata (origin/scope tagging at the hydration boundary) ships and
  consumers can refuse cross-scope bodies. **Any tool that injects shared memory into a
  privileged prompt must treat the content as untrusted** until that work lands.
- **Remote exposure is GA-blocked on RM3/RM4/RM8 (auth + replay hardening).** The
  remote-server project key (#813) + server-side interception (#819) mean a live remote path
  exists, but its auth/replay hardening is deferred. Closing this proposal **does not
  authorize E-PROD or remote multi-user exposure**; that remains gated on RM3/4/8.
- **Cross-client isolation is a load-bearing trust boundary.** Per-agent scoping depends on
  `AIMEE_HOOK_CLIENT` being forwarded honestly by the thin client and trusted per-request by
  the server (the #829 contract; the local/combined path env-fallback requires
  `AIMEE_HOOK_CLIENT` UNSET on a shared server). Before any multi-principal deployment the
  server must **authenticate the upstream thin client** so a rogue client cannot spoof an
  arbitrary `AIMEE_HOOK_CLIENT` to read another client's memory. The UDS hop is currently
  filesystem-trusted.
- **Platform matrix.** The real-time backstop (`harness_memory_watch`, inotify) is
  **Linux-only**; on macOS/Windows (where Claude/Gemini/Codex/Copilot are routinely run)
  external/manual edits are reconciled **only at session-start** (DB1-wins), leaving a
  between-sessions window in which a manual edit can be overwritten. A cross-platform watcher
  (FSEvents / `ReadDirectoryChangesW`) or a polling fallback is future-work; until then the
  at-write guarantee is Linux-only and the rest is session-start reconcile.
- **Drift observability.** The deferred `overwrite-divergent` / `removed_hash` audit
  counters are the **primary signal** that the backstop, reconcile, and canonicalization
  layers are silently disagreeing in production — prioritized first among the pre-GA items.
- **Hash domain (documented).** The SHA-256 `content_hash` is computed over the
  **canonicalized** body, not the raw on-disk bytes — an integrity check over the normalized
  representation, consistent with byte-non-exact rendering.

## Non-goals

- **Cross-machine sharing** (DB1 is single-host; future db2 sync — §1).
- Touching or migrating the existing kb `memory.*` path (db2). Untouched.
- Forcing agents to call a memory tool. Interception is enforcement; a voluntary tool is
  rejected as the primary mechanism (see Alternatives).

## Alternatives rejected (R1 #25)

- **A voluntary `aimee_store_memory` tool.** Cannot be *enforced* — agents won't reliably
  call it, and the whole point is to remove the agent's private store. Useful only as an
  optional extra surface.
- **Symlink/bind-mount the memory dir to a shared folder.** Gives shared files but no DB
  queryability, no curation/dedup, no per-client format rendering, no tombstone/provenance
  semantics. Insufficient for the goal.
- **Write-through to db2 instead of DB1.** Explicitly excluded by the requirement
  ("server DB, not kb DB"); also pulls in the kb dependency for a host-local feature.

## Risks / honest limits

- **Only agent-tool writes are intercepted at write time (R3 #2).** Non-tool/external
  changes — `vim`, IDE autosave, sync clients, container mounts, `python -c` via `Bash`,
  un-hooked agents, and exotic shell constructs (process substitution, subshells,
  `eval`/indirection) — are **not** prevented; they are reconciled to DB1 (DB1 wins) at the
  next session-start audit (§5) and logged (§7). The dominant `Write`/`Edit` path is covered
  exactly; an inotify/fanotify watcher that closes the at-write gap is a **follow-up, not a
  v1-correctness requirement** (R1 #3, R2 #4, R3 #2). This is stated, not hidden.
- **Canonicalization is not byte-exact (R3 #1/#21).** We store a client-neutral
  normalization and render per client, so a client re-reading its own write sees a
  semantically-equivalent — not byte-identical — file. The render codec is audited for
  round-trip fidelity (write→render→write→hash compare) and mismatches are logged; this is
  the deliberate cost of cross-client sharing under one `(project,name)` key.
- **Per-client deny contract is required (R3 #4/#10).** The deny-with-redirect is validated
  per client; any client lacking a deny-with-message hook protocol is out of v1 scope and
  listed as unsupported rather than silently mis-handled.
- **DENY-as-success wording** depends on the model interpreting the reason correctly;
  mitigated by live-run validation (R1 #14) but behavioral, not structural.
- **Shared-memory content is untrusted.** Agent-authored bodies are stored and re-read by
  other agents — a prompt-injection surface. Mitigations: body size cap, no auto-execution,
  documented file perms; deeper review/tagging is a follow-up (R1 #22).

## Tests

- DB1: upsert/get/list/search/tombstone (live-only by default)/idempotent SHA-256 hash;
  migration apply; bulk `_tombstone_prefix`.
- `name` validation: traversal/abs/null/edge-dot rejected; nested `topics/foo` accepted;
  **symlink/bind-mount escape rejected** (server + hook).
- Canonicalize↔render codec: round-trips per client; MEMORY.md write is **denied with the
  guidance message**, not stored.
- Interception across ≥2 clients (`AIMEE_HOOK_CLIENT`): Write/Edit/MultiEdit + Bash-redirect
  captured; per-file delete → tombstone; **`rm -rf memory/` → all rows tombstoned** (bulk).
- Per-client deny contract: Claude exit-0 JSON `permissionDecision:deny` is emitted (and the
  equivalent for each other supported client).
- Loop-bypass (`test_memory_redirect_no_loop`): aimee's re-materialize write with the hook
  installed does **not** re-enter; asserts the hook fires exactly once per agent write.
- Concurrency: two concurrent writes to the same `(project,name)` serialize under the mutex
  so DB1 row and on-disk file always agree (no upsert/rematerialize interleave — R4 #2).
- Reject-deny invariant: a denied MEMORY.md write/delete produces **no** tombstone and no
  reconcile-driven file change (R4 #6).
- Hook-absent: session-start with the hook uninstalled still reconciles on-disk → DB1 and
  emits the "interception OFF" warning (R4 #5).
- Failure: server-unreachable → spill written + fsync'd + tool **ALLOWED** (not blocked);
  next session-start consumes the spill.
- Reconcile precedence: disk-only→import; DB1-only→rematerialize; hash-mismatch→DB1-wins +
  divergence logged; tombstone→file removed; cross-filesystem target→spill not partial write.

## Review revisions (R1)

Roundtable design review (review mode, 2 rounds, degraded — one provider stalled, full
coverage). 28 items (8 blocking). All 8 blocking resolved in-design: dual-source-of-truth
→ DB1-authoritative + total interception + reconcile (§1); loop bypass → structural
syscall path + test (§1); Bash bypass → best-effort detection + documented limit (§3,
Risks); non-atomic UPSERT → server-POST single-txn (§3); path traversal → strict
validation + realpath confine (§3); failure policy → spill + non-block (§3); rematerialize
race → atomic write-before-deny (§3); deletes → tombstones (§2, §3). Adopted suggestions:
migration/import (§5), MEMORY.md as rendered index (§4), per-client raw-store + render
(§2, §4), module split (§3), idempotency hash (§2, §3), schema hardening (§2), single-host
scope documented (§1), alternatives section, untrusted-content risk.

## Review revisions (R2)

Roundtable review of this proposal (review mode, **heavily degraded** — 2 of 4 panelists
stalled upstream, 1 round completed; full coverage, no truncation). 8 items (4 "blocking").
Actioned: **R2 #1** — dropped the direct-SQLite fallback entirely; the **server is the sole
DB1 writer**, server-down → spill (§3), removing the last dual-writer race. **R2 #2** —
`name` validation now explicitly rejects `..` substrings + leading/trailing dots in addition
to the regex + realpath confine (§3 step 2). **R2 #4** — Bash-detection evasions (process
substitution, subshells, eval-indirection, symlinks) named explicitly as a documented v1
limit, inotify backstop is a follow-up not a v1-correctness requirement (§3 detection,
Risks). **R2 #5/#8** — schema_version migration strategy + UTF-8/binary handling specified
(§2). Clarified (panel misreads, no re-architecture): **R2 #3** — POSIX atomic `rename` +
write-before-deny means no partial/empty read window; advisory locks unnecessary, `fsync`
added for durability (§3 step 4). **R2 #6** — a write to a tombstoned `(project,name)` is an
intentional revive, not a `content_hash` resurrection (§2 tombstone semantics). Noted:
**R2 #7** — registry auto-discovery is a future enhancement; v1 ships a default registry +
unregistered-client warning (§6).

## Review revisions (R3)

Roundtable review of this proposal on the live **.254** deploy (review mode, **clean** —
`degraded:false`, 5/6 panelists, full coverage; the earlier R1/R2 runs were degraded only
because they hit a stale local server). 24 items (9 "blocking"). One blocking item was a
**false positive from a stale brief** I sent (R3 #3 reviewed the pre-R2 direct-SQLite
fallback the doc had already removed; the panel independently *recommended* the server-sole-
writer design, validating R2 #1). The other eight blocking items are folded in:

- **R3 #1 schema contradiction** — replaced "raw client bytes" with a **canonical,
  client-neutral** representation (`type`/`description`/`body`/`meta_json`) under one
  `(project,name)` row, rendered per client; resolves silent cross-client clobber (§2, §4).
- **R3 #2 "every mutation intercepted" too strong** — reframed to an explicit threat model:
  only agent-tool writes are intercepted; external changes reconciled to DB1 at session-start
  (§1, §5, Risks).
- **R3 #3 reconcile precedence undefined** — added a deterministic bidirectional policy,
  DB1-wins on hash mismatch (§5).
- **R3 #4 deny/liveness contradiction + per-client contract** — fixed the wrong "non-zero
  exit" wording, specified the per-client deny-with-message protocol (Claude exit-0 JSON;
  others or out-of-scope), chose liveness-wins + spill-always (§3).
- **R3 #5 bulk/dir deletes** — added a distinct destructive-op handler + `_tombstone_prefix`
  (§3).
- **R3 #6 MEMORY.md** — deny-with-guidance, never silent-discard (§4).
- **R3 #8 spill contract** — location/format/fsync/retention/partial-recovery specified (§3).
- **R3 #9 symlink + cross-fs rename** — realpath-chase before confine; `mkstemp` in the
  target dir; refuse-cross-fs→spill (§3).

Adopted suggestions: SHA-256 `content_hash` (#13), `project_id` worktree derivation (#14),
nested `name`/relpath + regex-vs-prose fix (#15), explicit scope on `AGENTS.md`/`CLAUDE.md`/
`.cursorrules` (#16), interception audit log (#12), hook ordering vs guardrails (#18),
NOT NULL + live-only-by-default tombstones (#19, #24), per-`(project,name)` mutex named
(#23), stateless-server/WAL note (#11), round-trip-fidelity audit (#21). **Process fix:**
regenerate the review brief from the current doc before any future round so we never review
stale text again.

## Review revisions (R4)

Convergence round on **.254** with a brief regenerated from the R3 doc (clean,
`degraded:false`, 4/6 panelists; the process fix worked — no stale-text false positives this
time). 15 items, blocking **down 9→6** and all of the six are correctness tightenings, not
architecture changes. Folded:

- **R4 #2 (genuine)** — the per-`(project,name)` mutex now spans the **whole
  upsert→rematerialize** sequence, so the DB1 row and its file update atomically; closes the
  TOCTOU where concurrent same-name writes could leave disk and DB1 disagreeing (§3 step 4).
- **R4 #6 (genuine)** — disambiguated **redirect-deny** (aimee performs the op) from
  **reject-deny** (deny + zero state change); a reject-deny (e.g. denied MEMORY.md delete)
  produces no tombstone and cannot mutate disk via reconcile (§3).
- **R4 #5 (genuine)** — reconcile is **hook-independent** and runs regardless; session-start
  verifies the hook and **fails loud** if it's missing ("interception OFF, import-only"),
  so a missing hook degrades safely, never silently (§5).
- **R4 #1** — added a manual `aimee harness-memory reconcile` trigger + an explicit
  durability boundary for the intra-session external-edit race (§5).
- Sharpened (already implied): explicit `realpath(dirname) ⊂ realpath(memdir)` symlink check
  (#3, §3), all-or-nothing bulk-tombstone txn + per-row audit (#4, §3/§4), spill-write-
  failure path (#8, §3), `$AIMEE_PROJECT_ID` non-security trust boundary + 0700 memdir
  creation (#9/#14, §6), named loop-bypass/concurrency/reject-deny/hook-absent tests (#15).

NITs #11 (EXDEV) and #12 (SHA-256) were panel misreads — the doc already specifies
refuse-cross-fs→spill and SHA-256 (the brief is a summary). Remaining open items
(canonicalization-codec enumeration #7, per-client deny edge cases #10) are **implementation
deliverables of P2/P3 with their own tests**, not proposal-level blockers.

**Convergence assessment:** three review rounds (R2 degraded, R3 clean, R4 clean) show a
falling blocking count (—/9/6) with no new architectural objections — only successively
finer correctness detail. The proposal is considered **settled**; remaining specificity
(canonicalization codec, per-client deny matrices) is owned by the phase PRs, each of which
gets its own code-level roundtable before merge.

**Erratum (timestamp domain).** The §2 schema sketch wrote `created_at/updated_at INTEGER`;
the implementation plan aligns them to DB1's house style — TEXT-ISO `(datetime('now'))`,
`deleted_at TEXT` — to match every neighbouring table. Identical semantics, no design change.
