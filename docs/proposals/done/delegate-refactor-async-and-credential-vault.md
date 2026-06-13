# Proposal: delegate refactor — async-only model + per-user credential vault

- **State:** done
- **Status refreshed:** 2026-06-13
- **Completion:** all phases shipped to `testing` — WP-A #210, WP-B #212, WP-C.0 #214, WP-C.1 #217, WP-C.2a #219, WP-C.2b #221, WP-C.2c(1) #226, WP-C.2c(2) #227, WP-C.2c(3) #232; WP-C.3 (per-principal credential pool re-key + vault-cred 429 cooldown + codex-oauth vault override) in #235, verify-local green, pending roundtable + merge.
- **Author:** JBailes
- **Date:** 2026-06-12
- **Charter roles:** Delegate (collapse the execution model to one path), Recall
  (unbounded result storage), Gate-Promote (default-safe vault rollout), Secure
  (per-user, client-key, envelope-encrypted credentials, fail-closed).

> **Revision note.** Rev. 1 was reviewed by the roundtable (R1) and came back
> *major-concerns* with severe, correct findings. The big three: (1) **WP-C was
> security-theater** — its per-user identity (`peer_uid`) is **not populated on
> the transport the vault uses**, and the credential use-path runs on a detached
> worker thread *after the connection is closed*, so no uid is in scope. (2)
> **WP-B silently broke the in-model `delegate` tool** — the MCP client and
> `cmd_diagnose` block on an *inline* result; my claim that "/v1 already polls"
> was false for the local MCP path. (3) **WP-A's real ceiling is the 256KB `/v1`
> response buffer**, not the struct cap, plus `sizeof(char*)` hazards and an
> unfreed resume path. Rev. 2 reshapes all three: WP-C gains a **hard-gate
> prerequisite** (wire SO_PEERCRED) and threads the uid through the closed-conn
> boundary; WP-B keeps the server **async-only** but preserves inline UX with a
> **client-side poll shim** in every inline consumer; WP-A addresses the 256KB
> ceiling and the full caller blast-radius. The crypto is pinned (HKDF, AES-GCM,
> AES-KW, AAD, fail-closed) per the security seat.
>
> **Rev. 3 (post-R2).** R2: WP-A / Simplicity / Integration endorsed; three seats
> held blockers. The headline security finding — convergent across the Security
> and Red-team seats — is that rev. 2 keyed the cached KEK on a **client-supplied
> `session_id`** (`compute_request_session_id` reads it from the request body,
> `server_compute.c:483-498`), which a local attacker can spoof to a victim's
> value → pull the victim's KEK. **D14 now keys the KEK cache strictly on the
> OS-attested `peer_uid`** and uses a **dedicated binary-safe, capacity-bounded
> KEK cache** (not the 64-slot NUL-terminated forge broker). Also: the in-model
> tool **already** returns a job_id by default (`server_mcp_delegate.c:67-68`,
> `mcp_tools.c:579`) so async-only is *less* disruptive than rev. 2 implied (D2
> re-baselined); two fire-and-forget skill callers (`skill_curator.c:141`,
> `skill_review.c:74`) + post-dispatch validation degradation join WP-B (D2/D6);
> D3's read-only-worktree contradiction is resolved (stray writes **fail the
> job**); and the transient-plaintext residual is widened honestly — a leased env
> credential lives in `agent->api_key` for the whole run (`server_compute.c:907`),
> now cleansed, while vault creds decrypt at the injection point only (D16).
>
> Rev. 3 also folds in a **new requirement**: webchat users need delegates, but
> the browser can't hold the key. Since `peer_uid` can't distinguish webchat users
> (they share the service-account UDS conn) and the browser is keyless, the vault
> is generalized to a **vault principal** — `uid:<peer_uid>` (CLI; SO_PEERCRED;
> client root key → HKDF) **or** `webuser:<username>` (webchat; app-authenticated;
> **login password → argon2id** server-side; browser stores nothing). The webchat
> backend asserts the authenticated user to aimee-server over the trusted
> `server.token` UDS channel (D9b/D18). This revises the old "UDS-only" non-goal:
> remote multi-user is now supported via the trusted webchat proxy; direct-TCP CLI
> multi-user stays future work (D17).
>
> **Rev. 4 (post-R3).** R3 hit mechanism, not shape (Recall/Simplicity endorsed;
> no seat challenged async-only/cap-removal/envelope-vault). Two convergent
> security blockers, both with clean fixes: (1) **argon2id is not in the deploy
> image** — all images are `debian:bookworm-slim` = OpenSSL 3.0, and argon2id
> needs 3.2+. D18 switches to **scrypt** (memory-hard, in OpenSSL since 1.1.0, on
> the 3.0 image, **no new dependency**). (2) **The `peer_uid`/`webuser:` identity
> had no enforcement** — `accept(g_listen_fd, NULL, NULL)` (server_http.c:1771)
> never captures `peer_uid`, and UDS requests skip the token check, so any local
> process could assert any `webuser:`. D10 now specifies the exact SO_PEERCRED
> capture + a threaded **attested-transport** enum; D9b/D18 gate the `webuser:`
> assertion behind the **`server.token` bearer** (the webchat backend's TCB
> credential) carried in an `X-Aimee-Webuser` header. Also: D3's isolation gate
> keeps `coord_task_id>0`/`branch` triggers (not role-only) and blocks stray
> writes at **source** (`write_enforce`), not a racy post-run git-diff; D4 keeps
> the bare CLI **in-process** (local durable record, not a server transport
> rewrite); D16 cleanses `agent->api_key` at a worker goto-label (not the
> unreachable `compute_ctx_free`); + TTL, RAND_bytes-fail-closed, and
> list_recent-no-heavy-columns are pinned.
>
> **Rev. 5 (post-R4).** R4 hit **zero blockers, all six seats endorse** — the
> design shape and mechanism are settled. Rev. 5 folds the 8 majors: **vault-FIRST
> precedence** (the env lease stuffs a secret into `agent->api_key` *before*
> `agent_resolve_auth`, so a vault hit must short-circuit it — else the vault is
> silently bypassed, D14); the **webchat transport** asserts `X-Aimee-Webuser` on
> the token-bearing *dispatch* path, not the `Authorization`-stripping `proxyV1`
> (D9b/WP-C.2); **`peer_uid` fail-closed + the 3-hop threading** across the zeroed
> `loopback_rpc` conn (D10/D11); **D8 pagination is built** (it was only echoed,
> never sliced); the **`delegate_worker` exit-path consolidation is sequenced as
> a behavior-preserving refactor first** (WP-B); + the resume-path single-exit
> free (D7) and the `leased_env`-is-a-name cleanse correction (D16). Per the
> author's direction, the **next review round runs via aimee delegates**
> (cross-family, full repo tools via the `review` role), not Claude subagents —
> deferred until the delegate-refactor is implemented (build-first, dogfood-later).

## Goal

1. **Remove the foreground/background delegate distinction entirely.** It is two
   divergent code paths (shared-vs-isolated worktree, open-connection-vs-job,
   interactive-vs-background priority, ephemeral-vs-durable) selected by a
   `background` flag. The **server becomes async-only**: every delegate is a
   durable, pollable, cancellable job; the synchronous open-connection path is
   deleted. Inline-result UX (which the primary model's tool loop needs) is
   preserved as a **client-side** poll-to-completion convenience, not a server
   path.
2. **Remove the 4096-char result cap** — and the *real* ceiling behind it. The
   struct `char result[4096]` only bites background delegates today; once every
   delegate is job-shaped (Goal 1) it would truncate **every** answer. But the
   binding ceiling on the canonical poll path is the **256KB `/v1` response
   buffer**, which must be addressed or large results are rejected, not stored.
3. **Move delegate credentials onto aimee-server, per-user, encrypted under a
   client-held key the server never persists** — so a stolen disk, another OS
   user, another session, or a compromised server process cannot recover a
   user's long-lived keys. This *refines* the prior client-held directive: the
   client holds the **key**, the server holds the **encrypted value**.

## §0 What already exists (verified)

- **The "background" machinery is the durable model.** Background delegates
  create a durable `agent_jobs` row (`server_compute.c:1660-1669`), lease (`:689`),
  heartbeat (`:1236`), cancel (`db1_agent_job_is_cancelled`), poll
  (`server_delegate_status.c`). WP-B keeps this and makes it the only single-
  delegate path.
- **Job columns are already unbounded.** `agent_jobs.prompt/.result` are `TEXT`
  (`db1/schema.sql:40`), bound with `sqlite3_bind_text(…,-1,…)`
  (`agent_jobs.c:102/135`). The 4096 cap is purely the struct (`agent_jobs.h:22-23,
  32,35`) + read-path `snprintf` (`db1_copy_col_text`, `db1_internal.h:27-31`).
  `agent_result_t.response` is **already** `char *` (`agent_types.h:251`).
- **OpenSSL is linked** (`CMakeLists.txt:963-964`): HMAC, EVP (incl.
  `EVP_aes_256_gcm`, `EVP_aes_256_wrap`, HKDF via `EVP_PKEY_derive`), base64url
  (`oauth_pkce.c:24`), `OPENSSL_cleanse` (`kb/http/kb_tls_serve.c:341`),
  `RAND_bytes`/`platform_random_bytes` (`posix/platform_random.c:7`). **No new
  dependency.**
- **The SO_PEERCRED primitive already exists** — `linux/platform_ipc.c`
  (SO_PEERCRED) and `mac/platform_ipc.c` (getpeereid) capture peer uid/gid — but
  **it is not wired into the server accept loop**, so `conn->peer_uid`
  (`server.h:133`) is unpopulated for served connections (no non-test `peer_uid =`
  assignment exists). WP-C.0 wires the existing primitive in.
- **A RAM-only session secret broker exists** as the *design model* for the new
  KEK cache. `forge_credentials.h` — "token lives IN MEMORY ONLY — never written
  to disk, never logged"; install/get/revoke + TTL + `OPENSSL_cleanse` wipe;
  thread-safe; process-global (reachable from a detached worker thread). WP-C does
  **not reuse it directly** (R2): it is a NUL-terminated string store (`strdup` /
  `snprintf("%s")` / `strlen`-wipe) that truncates a binary KEK at the first
  `0x00`, and it has only `FORGE_MAX_WS=64` slots. WP-C builds a **dedicated
  `vault_kek_cache`** that copies its RAM-only discipline but is binary-safe,
  `peer_uid`-keyed, and adequately sized (D14).
- **0600 at-rest precedents:** `db1/secrets.c`, `agents.json` (`agent_config.c:886`).
- **Injection point:** `agent_resolve_auth` (`agent_config.c:1310`, signature
  `(const agent_t*, char*, size_t)` — **no uid/session in scope**) builds the
  `Authorization` header; `agent_runtime.c:978` calls it, `:1014` fires the POST.
  Token buffers are **not** `OPENSSL_cleanse`d today.
- **Webchat is a keyless-browser, app-authenticated, shared-UDS path.** The
  browser talks to `aimee-webchat` (Go), which authenticates the user with
  username+password → session cookie (`webchat/chat.go:343` `requireAuth`,
  `webchat/pages.go:78`) and exposes the authenticated `username` via
  `currentUser` (`webchat/chat_sessions.go:28`) with real per-user isolation
  (`chat_sessions_test.go:216`: bob cannot touch alice's data). `aimee-webchat`
  reaches `aimee-server` over **UDS as a single service account** with a shared
  `server.token` bearer (`webchat/socket.go`, `webchat/openai.go:18`). So (a) all
  webchat users share one OS `peer_uid` — useless for distinguishing them; (b) the
  real identity is the app `username`; (c) the browser can durably hold **no**
  key. This drives D9b/D18 (the `webuser:` principal + password-derived KEK).
- **Three distinct durability backends exist (R1):** (1) server `agent_jobs` row;
  (2) CLI `--background` = a forked child writing `tasks/<id>.json` via
  `platform_delegate_run_background` (`cmd_agent_delegate.c:1347`) — *not* an
  `agent_jobs` row; (3) parallel/launch/coord = `db1_coord_job` keyed by
  `coord_task_id` (`server_compute.c:337/1011`, `server_coord_dispatcher.c:51`),
  flowing through the same worker.

## Decisions

### Execution model (WP-B)

- **D1 — One single-delegate model: async durable jobs.** Every single delegate
  creates an `agent_jobs` row, returns `{job_id, status:"pending"}`, and the
  connection is closed at dispatch. The synchronous `compute_respond`-over-
  `conn_fd` path (`server_compute.c:369-390`, `:1689`) is removed. `--background`
  (`cmd_agent_delegate.c:574/586`) and the request `background` field
  (`server_compute.c:1638`) are deleted; for one release the server
  **accepts-and-ignores** a stale `background` field to avoid client/server skew.
- **D2 — The in-model tool already returns a job_id; the client poll shim is for
  the remaining inline consumers (re-baselined per R2).** Correction: the in-model
  `delegate` tool **already defaults to async** — `handle_mcp_delegate_call`
  forces `background=1` (`server_mcp_delegate.c:67-68`), the server returns
  `{job_id, status:"pending"}`, and the tool description already says "Async by
  default, returns a job_id; poll delegate_status" (`mcp_tools.c:579`). So the
  dominant in-model path **needs no change** — it keeps returning a `job_id` and
  the primary model polls `delegate.status` across turns. WP-B simply **removes
  the explicit `background:false` "short-call" affordance** so there is one
  behavior. The client-side poll-to-terminal shim is therefore for the remaining
  **inline consumers** that block on a result today, all added to WP-B scope:
  - **`cmd_diagnose.c:288-310`** (forks `aimee delegate` + `waitpid`) → `job_id` +
    poll.
  - **The bare CLI `aimee delegate`** → defaults to `--wait` (poll to terminal,
    print result, surface a `failed` job's error text); `--no-wait` returns the
    `job_id`.
  - **`skill_curator.c:141` and `skill_review.c:74`** spawn `aimee delegate review
    --background` **fire-and-forget** (do not wait). They migrate to **`--no-wait`
    (returns `job_id`, stays non-blocking)** so they are not turned synchronous.
    An audit of every internal `aimee delegate … --background` invocation precedes
    flag removal.
  - Where a shim polls (CLI `--wait`), a turn/command abort mid-poll **cancels**
    the job (`jobs.cancel`) so no orphan holds a credential lease (D16).
- **D3 — Isolation gate keeps the coord/branch triggers; stray writes are blocked
  at SOURCE (corrects the R3 false premise).** `delegate_role_is_write`
  (`delegate_role.c:52-62`) is true for only `{code, refactor, prose, line-edit,
  lyric, hook}`; the **default role is `execute`** (`server_compute.c:721`), which
  is **not** a write role — yet coord work packets dispatch as `execute`
  (`server_coord_dispatcher.c:31`) and **legitimately write**, getting isolation
  *today* via `delegate_concurrent = coord_task_id>0` (`server_compute.c:1011`),
  **not** role. So keying isolation purely on role would break coord. The gate is
  therefore: **isolate-and-apply-back iff `write-role` OR `coord_task_id>0` OR an
  explicit `--branch`** (the existing non-role triggers are preserved); everything
  else runs worktree-less. The prompt **heuristic** (`delegate_allows_writes`) is
  removed from the *isolation* decision. For a worktree-less delegate, stray
  writes are prevented **at source**, not by a racy post-run `git status` (which
  cannot attribute a dirtied file to one of N concurrent delegates sharing the
  parent tree): enable the in-process write guard
  (`write_enforce=1` + `g_parent_write_root`, `agent_tools.c:53`) so a write is
  **blocked deterministically at the tool call** and **fails the job** with a
  clear error. (This also closes today's silent-discard at
  `server_compute.c:1509`.) If a role legitimately needs to write, it declares a
  write role / `--branch` / runs under coord, and gets isolation.
- **D4 — Three backends; the bare CLI stays IN-PROCESS (corrects R3).** (1)
  `agent_jobs` = the unified single-delegate server model (the in-model tool +
  webchat dispatch here and poll). (2) The bare CLI `aimee delegate` **runs the
  agent loop in-process today** (`cmd_agent_delegate.c:1355-1389`,
  `agent_http_init` + local `db1_init`) — it does **not** call the server. WP-B
  does **not** move it server-side (that would relocate credentials/worktree/cwd
  and is a much larger change); instead it keeps the in-process loop and reuses
  the existing local **durable `agent_jobs` record** the `--durable` path already
  writes (`cmd_agent_delegate.c:1364`). `--wait`/`--no-wait` are therefore
  **local** (block on / return the local job) — there is no server poll for the
  local CLI; the CLI `--background` fork-to-file
  (`platform_delegate_run_background`) is removed in favor of this local durable
  record. (3) **coord/launch (`coord_task_id`) is explicitly KEPT** as a separate
  *multi-delegate orchestrator* running delegates through the same worker. "One
  model" = **one single-delegate server model**; coord is the orchestrator above
  it, and the local CLI is in-process with a durable local record.
- **D5 — Per-machinery gating, spelled out (R2).** Today every durability item
  is gated on the single predicate `background_job_id > 0`, which D1 makes
  universal; D5 defines each item's new gate explicitly:
  - **Row create + terminal update** — **universal** (D1): every delegate is
    pollable. = two SQLite writes.
  - **Lease** (`db1_agent_job_take_lease`, `:689`) — taken for **write-role**
    delegates (worktree coordination + crash recovery). A read-only delegate is
    *unleased* but its row still records ownership via `lease_owner` at create and
    is reaped by the existing stale-row sweep; cancellation
    (`db1_agent_job_is_cancelled`) works regardless (it reads the row, not the
    lease). This is stated and tested, not assumed.
  - **`agent_set_durable_job`** (`:697`) — confirmed thread-local; if so it fires
    per worker thread for every delegate (correct under concurrency); if it is
    process-global it must be made per-thread first. **This is a pre-WP-B audit.**
  - **Heartbeat** (`:1237/:1272`) — throttled to ≥N seconds wall-clock (not per
    turn) for all delegates, so a short read-only delegate emits ~0 extra writes.
  A short read-only delegate therefore costs ~2 SQLite writes total. The added
  per-call write count is **benchmarked under a realistic in-model fan-out**
  against DB1's single-writer lock before this is called an improvement.
- **D6 — Pre-flight validation lifts inline; priority keeps coord BACKGROUND.**
  Under D1 the conn closes at dispatch, so the worker's ~10 post-dispatch
  `compute_error` points (persona `:756`, policy `:780`, config `:789`, routing
  `:864/:882`, credential `:914/:946/:981`, read-only+branch conflict `:1002`,
  worktree `:1049`) would otherwise degrade from an inline rejection to a
  "create-a-job-then-discover-the-failure-by-polling" experience. So the **cheap,
  request-only pre-flight checks** — persona presence, prompt presence/length,
  read-only+branch conflict — lift into `handle_delegate` **before**
  `db1_agent_job_create` + conn close, returning an inline 4xx (regression test).
  The genuinely post-dispatch failures (routing/credential/worktree) remain in the
  worker; the poll shim/CLI surface a `failed` job's **error text** (not a bare
  job id) so the caller sees the real reason. **Priority (R2 correction):** the
  explicit `priority` field already exists and is honored
  (`delegate_request_priority`, `:550-560`), and coord derives BACKGROUND from
  `coord_task_id > 0`. So: single delegates default **INTERACTIVE** (latency-
  sensitive `--wait` + in-model tool), and **coord/launch keeps deriving
  BACKGROUND** (`coord_task_id > 0` stays a BACKGROUND source) — it is **not**
  silently promoted to INTERACTIVE.

### Storage (WP-A)

- **D7 — Cap removal: no DB migration; NULL-safe; full blast radius.** Struct
  `char[4096]` → `char *` (drop `DB1_AJ_PROMPT/RESULT_LEN`). Add an allocating
  `db1_dup_col_text` **that always returns `strdup("")`, never NULL**, so the
  ubiquitous `field[0]` guards stay valid. Add `db1_agent_job_free`. Update
  **every** `db1_agent_job_get`/`list_recent` caller — `agent_tasks.c:406` (resume,
  currently leaks), `server_jobs_aux.c:108/133/158` (100-row array — loop-free),
  `server_delegate_status.c:20/32`, `cmd_agent_trace.c:278/300`,
  `server_compute.c:328`, **and the enumerated test files** (R2) —
  `test_db1_agent_job_heartbeat.c`, `test_db1_agent_jobs.c` and the other ~21
  `db1_agent_job_get` + 2 `list_recent` test sites — each paired with
  `db1_agent_job_free`. `db1_agent_job_free` **tolerates a zero-initialized
  struct** (`free(NULL)` is safe) so a failed `get` is safe to free. Ownership at
  the quarantine: `delegate_status_quarantine_degenerate_done`
  (`server_delegate_status.c:11-21`) mutates `job->result` **in place** — the
  rewrite must `free` the old `strdup`'d pointer **before** assigning the
  `strdup`'d replacement (not `snprintf` into a `char*`, whose `sizeof` is 8); and
  `db1_agent_job_free(&job)` is called at the **single exit** of
  `delegate_status_populate_job` after the cJSON build, only when `get` succeeded.
  **The resume path `agent_job_resume` (`agent_tasks.c:398`) reads `job.result`
  (`:434`) / `job.prompt` (`:442`) long after the `get` (`:406`) and has six
  early returns (R4):** convert them to a single `goto out` and free via
  `db1_agent_job_free(&job)` at the `out:` label **after** the last read. Whole
  suite under ASan.
- **D8 — Address the real 256KB `/v1` ceiling — and BUILD the pagination it
  needs (R4).** `SHTTP_RESP_MAX = 256KB` (`server_http.c:40`, `:1645`); the
  canonical `/v1` poll path **rejects** a larger result ("rpc response too large",
  `:958`). The `full_result`/`result_limit` params are parsed
  (`server_delegate_status.c:72-73`) but today only **echoed back** (`:78-81`) —
  the result is emitted **whole** at `:44`, so chunking does **not** exist yet.
  WP-A adds a real **`result_offset`** (cursor) alongside `result_limit`, threads
  both into `delegate_status_populate_job` (`:23`), slices
  `job.result[offset … offset+limit]`, and emits a `has_more`/`next_offset`
  marker so a >256KB result is fetched in bounded chunks over `/v1` (the UDS path
  can stream whole).
  Tested with a **1MB** round-trip over **both** UDS and `/v1`.
- **D9 — A sane stored-result ceiling; reject (not truncate) oversize input.** An
  unbounded result is a new memory/DoS surface. A configurable ceiling (default
  1MB) is enforced at the write-back (`compute_update_background_job`,
  `server_compute.c:332`): over-ceiling → truncate-with-marker + fail-flag, never
  silent. The 4MB inbound body cap (`server_http.c:1559`) changes from **silent
  truncation to an explicit 413**. **`db1_agent_job_list_recent` gains an
  `include_heavy=0` mode** that does **not** `SELECT`/`strdup` `prompt`/`result`
  (R3) — `handle_jobs_list` (`server_jobs_aux.c:107`) and the trace list
  serialize with `agent_job_to_json(…,0,0)` (heavy columns omitted) anyway, so
  list paths never allocate the now-unbounded bodies; full bodies are fetched one
  row at a time via `get`. State which 4096s are removed
  (`DB1_AJ_PROMPT/RESULT_LEN` only) vs **kept** (`MAX_EXEC_PROMPT_LEN`, the 2048
  partial/error buffers, `cron_jobs[].prompt`, and **`DB1_COORD_RESULT_LEN`** —
  the coord path's own 4096 result cap, which belongs to the separate coord
  backend (D4) and is **out of WP-A scope**; a follow-up if coord results also
  need to be unbounded).

### Credential vault (WP-C)

- **D9b — The vault is keyed on an abstract VAULT PRINCIPAL, not raw `peer_uid`
  (generalized for webchat).** A *vault principal* owns a vault file and keys the
  KEK cache. It is one of two **attested** identities:
  - **`uid:<peer_uid>`** — a local OS user on a **thin-client / CLI** connection,
    attested by SO_PEERCRED at UDS accept (WP-C.0, D10). KEK source: a client-held
    high-entropy random root key → HKDF (D12).
  - **`webuser:<username>`** — an authenticated **webchat** user. Webchat is a
    browser→`aimee-webchat`(Go)→`aimee-server` path where the *webchat backend*
    connects over UDS as a **single service account** (all webchat users share one
    `peer_uid`). Webchat authenticates the user itself (`webchat/chat.go:343`
    `requireAuth`) and **asserts** the authenticated `username` to aimee-server.
    **The assertion is enforced, not assumed (R3):** it rides a dedicated
    **`X-Aimee-Webuser: <username>`** header that aimee-server honors **only when
    the request also presents the valid `server.token` bearer** — the secret only
    the webchat backend holds (the 0600 `server.token` file, `webchat/openai.go:18`).
    Because today UDS skips the token check (`server_auth.c` allows any UDS conn),
    WP-C **adds** a `server.token` requirement *specifically* for any request
    carrying `X-Aimee-Webuser`. Holding `server.token` ⇒ trusted to assert any
    `webuser:` — that **is** the TCB boundary, and it is the webchat backend by
    construction (D18 states the residual). **Transport (R4 — verified):** webchat
    must assert the user on its **token-bearing dispatch path** (`webchat/api.go`
    `v1Request` / `methodRoutes` → first-class `/v1` routes), **not** the
    `proxyV1` ReverseProxy used for the *OpenAI surface*, which explicitly
    **deletes `Authorization`** (`webchat/openai.go:84`, "the UDS is a trusted
    local channel"). WP-C.2 includes the **webchat-side change**: on a
    vault/delegate dispatch, add the `X-Aimee-Webuser` header **and** forward
    `server.token` (do not strip it on that path). One live webchat delegate
    dispatch is traced end-to-end to confirm the entry point before WP-C.2 is
    written. KEK source: the login password → scrypt (D18). The browser holds
    **nothing**.
  Every downstream rule — vault-file ownership, KEK-cache key, AAD binding — is on
  the **principal string**, never a client-supplied `session_id`.
- **D10 — WP-C.0 hard gate: capture the attested identity + transport (exact
  mechanism, R3).** Today `accept(g_listen_fd, NULL, NULL)` (`server_http.c:1771`)
  captures **no** peer credential and the served `conn->peer_uid` is always 0.
  WP-C.0: in the accept path, for an `AF_UNIX` fd call
  `getsockopt(fd, SOL_SOCKET, SO_PEERCRED, …)` (reuse `linux/platform_ipc.c` /
  `mac/platform_ipc.c`), store `peer_uid` on `server_conn_t`, and compute an
  **`attested_transport`** ∈ {`UDS_PEERCRED`, `WEBCHAT_TRUSTED`, `TCP_BEARER`}:
  `WEBCHAT_TRUSTED` iff the conn presents `server.token` **and** an
  `X-Aimee-Webuser` header; `UDS_PEERCRED` for a plain UDS conn; `TCP_BEARER`
  otherwise. **The principal must survive the `loopback_rpc` boundary, which
  builds a `memset(&fake,0,…)` `server_conn_t` (`server_http.c:925-933`) — so
  every `/v1` request reaches `server_dispatch` through a conn whose `peer_uid` is
  0 (R4).** WP-C.0 therefore threads the attested identity as **three named,
  separately-tested hops**: (1) a thread-local `g_rpc_conn_peer_uid` +
  `g_rpc_conn_principal` set in `handle_conn` from the SO_PEERCRED capture; (2)
  `loopback_rpc` propagates them into the synthesized request rather than zeroing
  them; (3) `create_compute_ctx` copies the principal + transport into
  `compute_ctx_t`. **Fail-closed (R4):** the `uid:` vault path **refuses to
  operate when `peer_uid == 0`** (an un-attested conn) rather than silently acting
  as `uid:0` — so a missed hop is a hard error, not a privilege collapse.
  **`vault.unlock` / the `uid:` principal require `UDS_PEERCRED` with a non-zero
  uid; `webuser:` requires `WEBCHAT_TRUSTED`; `TCP_BEARER` gets no vault** (D17).
  **WP-C.0 is a scheduled, owned prerequisite, end-to-end tested (two uids → two
  principals; the zeroed-loopback conn still resolves the real uid; a spoofed
  `X-Aimee-Webuser` without `server.token` is rejected; `peer_uid==0` → refuse,
  not uid:0) before any vault body.**
- **D11 — Thread the *attested PRINCIPAL* across the closed-conn boundary; it is
  the ONLY security key.** The credential is resolved on a **detached worker
  thread after `conn_fd` is closed**, where neither the conn nor a thread-local
  survives. So resolve the vault principal (D9b) **while the conn is live** —
  `uid:<peer_uid>` from SO_PEERCRED for CLI, or `webuser:<username>` from the
  webchat-asserted, `server.token`-trusted header for webchat — **never** the
  client-supplied `session_id` (body-controlled at `server_compute.c:483-498` and
  spoofable). Capture the **principal string** into `compute_ctx_t` at
  `create_compute_ctx` and thread it **explicitly** through `delegate_dispatch →
  worker → agent_runtime → agent_resolve_auth` (new param; not a thread-local).
  The **principal is the single key for BOTH** the vault file (ownership) **and**
  the KEK cache (D14). Concretely: `compute_ctx_t` gains `char
  principal[…]` + `attested_transport` fields, set in `create_compute_ctx` while
  the conn is live; **`agent_resolve_auth` has SIX production callers** that all
  gain a `principal` param and the boundary cleanse (D16):
  `server/agent_runtime.c:978`, `posix/agent_runtime.c:448`,
  `server/server_agent.c:765`, `server/openai_chat.c:695`, plus the remaining two
  server callers and the test stubs — enumerate and update all.
- **D12 — KEK source #1 (CLI / `uid:` principal): high-entropy random root key →
  HKDF.** The thin client generates a 32-byte random **root key** (stored
  client-side, 0600 / OS keychain) — *not* a human passphrase. KEK =
  HKDF-SHA256(root_key, salt=per_principal_salt, info="aimee-vault-kek-v1"), 32
  bytes. No memory-hard cost is needed (the input is already high-entropy). The
  envelope's `kdf_version` records HKDF; KEK length is validated == 32.
- **D18 — KEK source #2 (webchat / `webuser:` principal): login password →
  scrypt; browser stores nothing.** The browser cannot hold a key, so a webchat
  user's KEK is derived **server-side from the webchat login password** the user
  already presents to `requireAuth` — via **scrypt** (`EVP_KDF "SCRYPT"` /
  `EVP_PBE_scrypt`). **scrypt, not argon2id (R3):** all deploy images are
  `debian:bookworm-slim` = **OpenSSL 3.0**, where argon2id (added in 3.2) does
  **not** exist; scrypt has shipped since OpenSSL 1.1.0, so it is memory-hard
  **and** available with **no new dependency or image bump**. Params: `N=2^17`,
  `r=8`, `p=1` (tunable), recorded with the per-principal salt under
  `kdf_version` so they can evolve. Derivation happens at **login** (password
  transiently in hand), the KEK is cached in the `vault_kek_cache` keyed by
  `webuser:<username>` (TTL'd, wiped on logout/expiry, never to disk), and the
  **password is `OPENSSL_cleanse`d** right after. **A password change re-wraps the
  vault** (`vault.rekey`: unwrap each DEK with the old KEK, re-wrap with the new —
  DEKs/ciphertext untouched, so creds are not re-encrypted), inside the
  password-change flow while both passwords are in hand. **Threat statement:** the
  webchat backend (already authenticating + proxying every webchat user, and the
  holder of `server.token`, D9b) is in the TCB for its users — a compromised
  webchat backend can capture a password at login and impersonate that webuser to
  the vault. Scoped per-webuser; it does **not** weaken the CLI `uid:` path, the
  at-rest store, or other webusers. At rest, scrypt-wrapped ciphertext still
  resists disk/backup theft (scrypt's memory-hardness is the offline-attack
  defense for the lower-entropy password input).
- **D13 — Envelope crypto, pinned and fail-closed.** Per **principal**: a random
  `per_principal_salt` (in the vault-file header) → one KEK (D12 for `uid:`, D18
  for `webuser:`). Per credential: a random **DEK** (32 bytes), the secret
  AES-256-GCM-encrypted under the DEK with a **fresh 96-bit nonce** and **AAD =
  `principal|agent|cred`** (the principal string `uid:N` or `webuser:name` — binds
  identity → blocks vault-file/row substitution/rollback); the DEK **wrapped under
  the KEK with AES-KW (RFC 3394, `EVP_aes_256_wrap` — no wrap-nonce hazard)**. The
  vault file stores per cred `{agent, cred, wrapped_dek, cred_nonce, ciphertext,
  gcm_tag}` and per principal `{kdf_version, per_principal_salt}` — **never KEK,
  DEK, or plaintext**.
  **Nonce/DEK invariant (R2):** every `vault.set`/update/rotate generates a
  **fresh random DEK (32B) AND a fresh 96-bit nonce** from `RAND_bytes` — a
  `(DEK, nonce)` pair is never reused and a secret is never re-encrypted under an
  existing DEK. **`RAND_bytes`/KDF failure is fail-closed (R3):** a non-1 return
  from `RAND_bytes` or the scrypt/HKDF derivation **aborts** the vault op with
  "vault: entropy/KDF unavailable" — never proceed with a zero/short key. Decrypt
  uses plaintext **only if `EVP_DecryptFinal_ex` returns 1** (GCM tag verified);
  on any failure, `OPENSSL_cleanse` the output + DEK and return error.
- **D14 — A dedicated KEK cache, keyed by PRINCIPAL, binary-safe and capacity-
  bounded (fixes the R2 convergent blocker).** The KEK is cached in a
  **purpose-built `vault_kek_cache`** — *not* the `forge_credentials` broker,
  which (a) is a NUL-terminated string store that truncates a binary KEK at the
  first `0x00`, and (b) has only `FORGE_MAX_WS=64` slots (`forge_credentials.c:17`)
  → a 65th unlock would fail or evict an in-use KEK. The new cache copies the
  broker's RAM-only discipline (never to disk, never logged, `OPENSSL_cleanse` on
  evict/revoke, TTL'd, thread-safe) but is **keyed by the attested principal
  string** (D11 — never a client-supplied `session_id`), holds the **raw 32-byte
  KEK** (no base64/decode buffer), and is sized for the expected concurrent
  principal count with a **reject-don't-evict** over-capacity policy. **TTL (R3):**
  a wall-clock `VAULT_KEK_CACHE_TTL` (default **900 s**) after which the KEK is
  `OPENSSL_cleanse`d and evicted; a delegate after expiry hits the fail-closed
  "vault locked" path (D15), not a stale key (test: a KEK is wiped at TTL). **Unlock
  trigger differs by source:** the CLI presents its root key via
  `vault.unlock(root_key)` (UDS-only, **refused over direct TCP**); webchat
  derives the KEK automatically from the password at `requireAuth` login (D18) —
  no separate unlock step. Per delegate call the worker fetches the KEK by
  **principal**, unwraps the DEK, GCM-decrypts into a **transient** buffer at the
  injection point, fires the call, and `OPENSSL_cleanse`s the plaintext — the
  decrypted credential is **never copied into the long-lived `agent->api_key`**
  (D16). **Vault-FIRST precedence (R4):** the `(principal, agent, cred)` vault is
  consulted **before** the env-pool lease at `server_compute.c:893`, and a vault
  **hit short-circuits the env lease entirely** — today the leased env secret is
  stuffed into `target_agent->api_key` at `:907` *before* `agent_resolve_auth`
  (the vault injection point) is ever reached, so without this the vault would be
  silently bypassed for any agent that still has an env-pool credential.
- **D15 — Fail CLOSED on a locked vault; vaulting removes the *per-principal*
  copy only.** Distinguish **"no vault entry"** (legitimate → fall back to
  env/agents.json, *logged*, no values) from **"entry exists but locked / KEK
  expired"** (→ **hard error** "vault locked: run `aimee vault unlock`", never a
  silent downgrade). **Vaulting a credential removes it only from that
  principal's own `agents.json`/config (R3)** — it must **not** mutate the
  process-global env or a shared agent config, since `delegate_credentials.c` is a
  process-global pool keyed by `(agent,cred)` until WP-C.3 re-keys it, and
  deleting a shared env var would break *other* principals. A credential left in
  the global env is documented as **not vault-protected** and surfaced for
  migration. Every fallback is logged so downgrades are observable.
- **D16 — Cleanse on every path; honest (wider) residual.** Two distinct
  plaintext lifetimes, stated truthfully (R2 corrected rev. 2's "one call"):
  - **Env/agents.json creds (existing path):** the leased secret is copied into
    `target_agent->api_key` (`server_compute.c:907`) where it lives for the
    **entire worker run**. `target_agent` points into the **per-call stack `acfg`**
    resolved by `agent_route_with_caps` (`server_compute.c:872`) — it is **not**
    owned by `compute_ctx`, so `compute_ctx_free` cannot reach it (R3). Instead a
    **single `goto out` label in the worker body** `OPENSSL_cleanse`s
    `target_agent->api_key` (the secret **value**) at **every** worker exit, while
    `acfg` is still in scope, and `fb_agent.api_key` is cleansed after the
    fallback POST — closing a leak that exists today. (`leased_env`,
    `server_compute.c:896`, is the env-var **name** — a block-local non-secret —
    and is *not* on the cleanse list, R4.)
  - **Vault creds (new path):** decrypted **at the injection point only**, into a
    transient buffer used for the single POST, then cleansed — **never** copied
    into `agent->api_key`. Lifetime = one provider call.
  A single `goto out` cleanup in `agent_runtime` cleanses `auth_header` on
  **every** return path (many early-error returns sit between resolve and the
  POST); the inner token buffer is cleansed inside `agent_resolve_auth` before
  return (all six callers, D11); the `X-Aimee-Session-Key` `skey[256]`
  (`server_http.c:1442`) and any copied headers/body are cleansed. **Cancellation
  → lease release (R2):** since cancellation is cooperative
  (`db1_agent_job_is_cancelled` polled by the worker, lease taken at
  `server_compute.c:901`), `jobs.cancel` must force lease release at the worker's
  next checkpoint and bound the in-flight POST timeout for a cancelled job, so an
  aborted MCP-shim poll cannot leak a credential lease (test: cancel mid-run →
  lease released within the bound). **Residual, honestly (R3-widened):** the
  exposure window for a KEK is the **`VAULT_KEK_CACHE_TTL`**, and a memory dump
  exposes **every principal currently unlocked within that window** (each KEK
  decrypts that principal's whole co-resident vault) — not "one principal" as
  rev. 3 implied. The unlocking secret (root key / password) transits on each
  unlock. What is guaranteed: **nothing persists** (KEK never written, vault at
  rest is ciphertext), and the at-rest store + any *locked* principal stay
  protected. A **strict mode** (no cached KEK; the `uid:` root key supplied per
  `vault.unlock`-and-run op, window = one call) is offered as a config option and
  is the recommended posture for the `uid:` path under the maximal threat model;
  the `webuser:` path cannot be strict (re-deriving from the password per op would
  require re-typing it), so its window is the cache TTL.

## Threat model (WP-C, after D9b–D18)

| Threat | Defense |
|---|---|
| Other local OS user reads keys | One **0600 vault file per principal** (`.vault/<principal>.json`, ownership checked against the asserted principal, not the shared service uid) + at-rest ciphertext. NB UDS has no token gate today, so the socket-file permissions + the principal check (not crypto) are what stop a *peer* local process; the at-rest file stays ciphertext regardless (D9b/D10/D13). |
| Stolen disk / `pg_dump` / backup | At rest = AES-256-GCM ciphertext + AES-KW-wrapped DEK; **KEK never on the box** (D13). Useless without the client root key (`uid:`, HKDF) / the user's password (`webuser:`, **scrypt** resists offline attack). |
| Cross-principal use | Principal access-control check **and** per-principal KEK (per-principal salt) — another principal's key can't unwrap (D11/D13). AAD binds `principal\|agent\|cred` so a swapped vault/file fails the tag (D13). |
| Compromised aimee-server / memory dump | KEK never persisted; vault at rest ciphertext; **every principal unlocked within `VAULT_KEK_CACHE_TTL`** + one in-flight plaintext is exposable — nothing long-lived (D16). Strict mode shrinks the `uid:` window to one call. |
| Compromised **webchat backend** | Webchat is in the TCB for its own users (it already authenticates + proxies them): it can capture a password at login and impersonate that webuser to the vault. Scoped per-webuser; does **not** affect the CLI `uid:` path, the at-rest store, or other webusers (D18). Inherent to a keyless browser. |
| Locked-vault downgrade | Fail-closed: entry-exists-but-locked errors; no silent env fallback; vaulting removes the **per-principal** config copy (not the shared global env) (D15). |

## Phasing (each independently shippable, default-safe)

- **WP-A — Cap removal + 256KB ceiling + sane stored ceiling.** No behavior
  change beyond larger results; unblocks WP-B. Ships first.
- **WP-B — Async-only unification.** Requires the client-side poll shims (D2) to
  land **with** the server sync-path deletion (no window where the in-model tool
  is broken). Depends on WP-A. **Sub-sequence (R4):** the `delegate_worker`
  (`server_compute.c:681-1543`, ~862 lines, 15 `compute_ctx_free`+return exits, 6
  scattered `delegate_credentials_release` sites) is touched by D5 (re-gate
  lease), D6 (lift pre-flight checks), and D16 (cleanse at a goto-out) at once —
  a high-risk single-function change. Land a **behavior-preserving `goto out`
  exit-path consolidation FIRST** (one cleanup label; lease released exactly once;
  `api_key` cleansed; job marked `failed` with real error text), with a test that
  injects a failure at each of the ~10 post-dispatch error points, **then** layer
  D5/D6/D16 on top — not as one diff.
- **WP-C.0 — Wire SO_PEERCRED** (hard gate; small, independently useful).
- **WP-C.1 — Encrypt/decrypt core (CLI `uid:` principal)**:
  `vault.set`/`unlock`/`lock`/`list`/`delete` + use-path decrypt for the `uid:`
  principal over UDS, 0600 per-principal file substrate, the dedicated
  principal-keyed binary `vault_kek_cache` (D14), HKDF+AES-GCM+AES-KW, AAD,
  fail-closed, full cleanse (D16). Smallest viable secure slice. Default-off /
  coexistence (D15).
- **WP-C.2 — Webchat `webuser:` principal (the keyless-browser path).** Add the
  trusted `webuser:` assertion from the webchat backend to aimee-server; derive
  the KEK from the login password via scrypt at `requireAuth` (D18); cache by
  `webuser:<username>`; `vault.rekey` on password change. Reuses the entire WP-C.1
  crypto core — only the identity source and the KEK source (scrypt vs HKDF)
  differ. Webchat UI to `set`/`list`/`delete` vault creds.
- **WP-C.3 — Pool re-key + migration (follow-up):** move
  `delegate_credentials.c` leases/cooldown/health from `(agent,cred)` to
  `(principal,agent,cred)`; migrate codex-oauth (vault entry overrides the disk
  token; orthogonal otherwise). Separable from the crypto core.

## Non-goals

- **D17 — Remote multi-user is supported ONLY via the trusted webchat backend
  (`webuser:` principal, D9b/D18).** Direct-TCP thin-client multi-user (a remote
  `aimee` CLI authenticating its own OS user over the network, e.g. via
  OIDC/bearer-with-user-claim) is **future work**; a raw-`peer_uid` principal is
  refused over direct TCP, and CLI root-key push is UDS-only.
- No HSM/external KMS — the "KMS" is the client holding the root key (`uid:`) or
  the user holding their password (`webuser:`).
- No protection of a credential *during its own provider call* (transient
  plaintext is unavoidable unless the client proxies the call).
- No change to provider wire protocols or the agent loop itself.

## Tests

- **WP-A:** a 1MB delegate result round-trips create → store → poll → client with
  zero truncation over **both** UDS and `/v1` (paginated); `db1_dup_col_text`
  never returns NULL; every `get`/`list_recent` caller (incl. `agent_tasks.c:406`
  resume and the 100-row `server_jobs_aux.c` array) frees with no leak/double-free
  under **ASan**; the quarantine rewrite stores correctly; oversize input → 413,
  oversize result → truncate-with-marker.
- **WP-B:** the MCP `delegate` tool returns a real answer (poll shim), and a
  turn-abort cancels the job; `cmd_diagnose` works via job_id+poll; bare CLI
  `delegate` blocks via `--wait` and `--no-wait` returns a job_id; the
  `skill_curator`/`skill_review` callers stay non-blocking (`--no-wait`);
  `cmd_diagnose` works via job_id+poll and surfaces a `failed` job's error text; a
  write-role delegate isolates + applies back; a **read-only-role delegate that
  writes FAILS the job** (no silent discard/apply); a cancelled job releases its
  credential lease within the bound; pre-flight prompt/persona validation returns
  inline 4xx; a stale `background` field is accepted-and-ignored; coord/launch
  tasks remain BACKGROUND priority.
- **WP-C.0:** two distinct OS uids over the same UDS resolve to distinct
  `peer_uid`/principals; the KEK cache and vault access key on `peer_uid`, and a
  spoofed body `session_id` does **not** change which vault/KEK is reached.
- **WP-C.1:** `vault.set`→`unlock`→delegate-call decrypts and authenticates; the
  at-rest file contains **no plaintext and no KEK** (grep); the `vault_kek_cache`
  holds a **raw 32-byte KEK byte-exact** (incl. embedded `0x00`) and `OPENSSL_cleanse`s
  it fully on evict/revoke/TTL; over-capacity unlock **rejects** (never evicts an
  in-use KEK); a second OS uid cannot read uid X's file; a session for uid X
  cannot reach uid Y's vault/KEK even given Y's `session_id` or Y's file; a
  tampered ciphertext / swapped row (AAD mismatch) **fails closed**
  (`EVP_DecryptFinal_ex != 1`); every `vault.set` yields a distinct DEK **and**
  nonce; the transient token buffer **and** `agent->api_key` are `OPENSSL_cleanse`d
  on every path incl. an injected failure between resolve and POST (assert
  zeroed); plaintext credentials **never** enter the KEK cache; a **locked** vault
  errors (no env fallback); a **missing** entry falls back to env (logged);
  root-key push over TCP is refused.
- **WP-C.2 (webchat):** a `webuser:` vault round-trips set→(login-derived KEK)→
  delegate-call; the KEK derives from the login password via scrypt and the
  **password is cleansed** after derivation; the browser/`localStorage` holds no
  key material (assert nothing key-shaped is ever sent to the client); a password
  change `vault.rekey`s without re-encrypting the credentials and the old KEK no
  longer unwraps; webchat user `alice` cannot reach `bob`'s vault (principal
  isolation) even though both share the service-account `peer_uid`; the
  `webuser:` assertion is only honored on the `server.token`-authenticated webchat
  channel (a raw client cannot assert an arbitrary `webuser:`).
