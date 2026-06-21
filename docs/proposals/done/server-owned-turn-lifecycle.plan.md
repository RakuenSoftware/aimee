# Implementation plan: server-owned turn lifecycle (Phase 1)

Diff-level plan for the roundtable-approved design
(`server-owned-turn-lifecycle.md`). All server-side work ships in **one PR**
(WP-2 and WP-4 are interdependent). Webchat/SPA work (Step 8–9) may land in the
same PR or a fast follow, but the server is correct and testable on its own
after Step 7.

## Ordering (and why)

1. Ring schema + `presence_session_owner` accessor (no behavior change).
2. `stream_event` / `stream_event_usage` restructure: ring publish unconditional,
   `conn_alive` guards only the socket write; coalescing. (WP-1)
3. Drop the `>1` attachment gate. (WP-1)
4. Per-turn cancel registry (new module). (WP-4)
5. CLI read loop: `O_NONBLOCK` + `poll` + cancel check; remove disconnect-kill;
   single-reap. (WP-2 + WP-4)
6. In-process agent path: thread the cancel flag. (WP-4)
7. Triggers: `session.close`, `server_shutdown`, `chat.graceful_cancel`. (WP-4)
8. Webchat backend (`chat.go`): 202 + events proxy; drop `r.Context()` leash. (WP-5)
9. SPA (`frontend/src/pages/Chat.tsx`): events render source + cursor + gap. (WP-5)
10. Tests + build wiring. (WP-6)

Steps 4–5 must compile/pass together (the loop loses its only interrupt in 5;
4 provides the new one). Land 2 before 5 so the ring is the sink before the loop
stops gating on `conn_alive`.

## Step 1 — Ring schema + owner accessor

**`src/headers/presence.h` / `src/server/presence.c`**
- Add `int presence_session_owner(const char *session_id, char *out, size_t n);`
  returning 1 + the owner principal (the `owner` field already at `presence.c:53`)
  or 0 if unknown. Used by `chat.graceful_cancel` authz (Step 7).
- No schema change to the ring itself; the new `turn_delta` `kind` values
  (`text`/`thinking`/`tool_call.<phase>`/`usage`) are carried in the existing
  `data_json` payload — purely a producer/consumer convention (Steps 2, 9).

## Step 2 — `stream_event` / `stream_event_usage` restructure (WP-1, fixes R3 blocker)

**`src/posix/server_compute.c:78` `stream_event`**
- **Remove** the top `if (!cctx->conn_alive) return -1;` (`:80`).
- Build the event JSON as today.
- **Ring publish first, unconditionally:** if `cctx->presence_session[0]`, publish
  to the ring. Apply the coalescing rule:
  - `text` deltas: append to a per-`cctx` text accumulator
    (`char *delta_buf; size_t delta_len; uint64_t delta_first_ms;`); flush to the
    ring as one `turn_delta {kind:"text",content:…}` when the buffer exceeds
    `RING_TEXT_COALESCE_BYTES` (≈2KB) or `RING_TEXT_COALESCE_MS` (≈50ms) since
    first byte.
  - Any **non-text** event (`thinking`, `tool_call.*`, `usage`, `session`,
    `turn_end`/`done`): **flush the pending text buffer first**, then publish the
    event immediately as its own `turn_delta`/boundary event. Guarantees no
    reordering.
- **Then the socket write, guarded:** `if (cctx->conn_alive) { write_all(conn_fd…);
  on failure set conn_alive=0; }`.
- Add a `stream_flush_text(cctx)` helper; call it before every non-text publish
  and once at turn teardown.
- **Per-cctx serialization (fixes reentrancy/reorder race).** The coalescing
  buffer is per-cctx mutable state and tool/usage events can be emitted
  concurrently (parallel tool execution). The whole body of `stream_event`,
  `stream_event_usage`, and `stream_flush_text` runs under the **existing
  `cctx->write_mutex`** (`server_compute_impl.h:25`, already taken at `:93`) —
  extended to cover the ring publish + buffer mutation, not just the socket
  write. This makes each emit atomic per cctx, so non-text events can never
  interleave ahead of buffered text. The mutex is per-cctx (not global), so
  distinct turns do not contend.

**`src/posix/server_compute.c:123` `stream_event_usage`**
- Same restructure: publish `turn_delta {kind:"usage",in,out,cost,usage_kind}` to
  the ring unconditionally (after flushing pending text), then the
  `conn_alive`-guarded socket write.

**Memory:** free `delta_buf` in the worker teardown paths (all
`compute_ctx_free` sites already centralize cctx cleanup — add the buffer to
`compute_ctx_free`).

## Step 3 — Always mirror to the ring (WP-1)

**`src/server/server_compute_async.c:440`**
- Replace the `presence_attachment_count(delta_session) > 1` condition with
  `delta_session[0] != '\0'` so `presence_emit_deltas` is set for every
  presence-tracked turn (single or multi attach). Keep the `!locked` branch that
  copies session/turn id into `cctx`.

## Step 4 — Per-turn cancel registry (WP-4)

**New: `src/server/turn_registry.c` + `src/headers/turn_registry.h`**
```
typedef struct { char session_id[PRESENCE_SESSION_ID_MAX];
                 char turn_id[PRESENCE_TURN_ID_MAX];
                 _Atomic int cancel; pid_t child_pid; int reaped;
                 int in_use; pthread_t owner; } turn_entry_t;
void          turn_registry_init(void);
turn_entry_t *turn_registry_publish(const char *sid, const char *turn_id); // NULL on collision
void          turn_registry_set_child(turn_entry_t *e, pid_t);
int           turn_registry_cancel(const char *sid, const char *owner_principal); // authz-checked
turn_entry_t *turn_registry_find(const char *sid);
void          turn_registry_mark_reaped(turn_entry_t *e);
void          turn_registry_clear(turn_entry_t *e);   // after reap + turn_done
int           turn_registry_cancel_all(void);         // shutdown: set all flags, return count
int           turn_registry_sweep_dead(void);         // crash backstop
```
- Bounded fixed table (`TURN_MAX` = `PRESENCE_MAX`), one `pthread_mutex_t`.
- **One-turn-per-session invariant.** The presence turn-lock
  (`presence_turn_acquire`) already guarantees ≤1 in-flight turn per session, so
  keying on `session_id` is safe. `turn_id` is stored for observability and for
  matching the events stream. **Collision is a bug:** `turn_registry_publish`
  returns `NULL` if the slot is already `in_use`; `chat_stream_worker_pooled`
  treats `NULL` as a hard error (logs, emits a turn error, does not dispatch) —
  never a silent overwrite that would orphan a `child_pid`. A regression test
  covers the collision path.
- **Cancel visibility:** `cancel` is `_Atomic int`; `turn_registry_cancel` does
  `atomic_store(&e->cancel, 1, seq_cst)`, readers `atomic_load`. No torn reads,
  no missed write; the hot-path reader needs no lock.
- **Lock discipline:** the registry mutex is a **leaf** — never held while
  acquiring a presence or compute lock, and **no caller may hold a presence or
  compute lock across** any registry call. `turn_registry_cancel` /
  `_cancel_all` acquire the registry mutex, set the atomic flag(s) and read
  `child_pid` under it, then release **before** any `kill`/`waitpid` (which the
  owning worker performs). `_cancel_all` iterates under the mutex setting flags
  only (no signalling inside the lock).
- **`sweep_dead`** (crash backstop): acquires the registry mutex, and for each
  `in_use` entry whose `owner` thread is no longer live
  (`pthread_kill(owner,0)==ESRCH`) reaps `child_pid` if `!reaped` and clears the
  entry. Called from the existing periodic compute-thread bookkeeping; documented
  as defense-in-depth, not the primary path.
- Hot-path read: the CLI/agent worker caches its `turn_entry_t*` (returned by
  `publish`) and reads `e->cancel` via `atomic_load`, avoiding per-tick locking.

**`src/server/server_compute_async.c` `chat_stream_worker_pooled`**
- `turn_entry_t *e = turn_registry_publish(sid, turn_id);` immediately before the
  worker run (entry exists before any event can fire). If `e == NULL`
  (collision — should be impossible under the turn lock): log, emit a turn error,
  skip dispatch. Record `e->owner = pthread_self()`. Pass `e` (or `sid`) to the
  worker so it caches the pointer.
- After the worker returns and `presence_turn_release`/`turn_done`:
  `turn_registry_clear(e)`.

## Step 5 — CLI read loop interruptible; remove disconnect-kill (WP-2 + WP-4)

**`src/posix/server_compute.c` (fork at `:1113`, loop at `:1233`, kill at `:1353`)**
- After fork in parent: `turn_registry_set_child(sid, pid);` and
  `fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);`.
- Replace `while (cctx->conn_alive && fgets(line, …, fp))` with a `poll()`-driven
  loop on `out_pipe[0]` (drop the `FILE*`/`fgets`; read into a fixed buffer):
  ```
  for (;;) {
    if (atomic_load(&e->cancel)) { cancelled = 1; break; }   // top-of-tick
    struct pollfd pfd = { out_pipe[0], POLLIN, 0 };
    int pr = poll(&pfd, 1, 200);
    if (pr == 0) continue;                            // tick: re-check cancel
    if (pr < 0) { if (errno==EINTR) continue; break; }
    if (atomic_load(&e->cancel)) { cancelled = 1; break; }   // re-check before read
    if (pfd.revents & (POLLIN|POLLHUP)) {
      ssize_t n = read(out_pipe[0], buf+blen, sizeof(buf)-blen);
      if (n == 0) { eof = 1; break; }                 // provider closed: done
      if (n < 0) { if (errno==EAGAIN||errno==EINTR) continue; break; }
      blen += n;
      // line splitter: scan buf[0..blen) for '\n'; for each complete line,
      // parse JSON + dispatch through the existing stream_event path; then
      // memmove the remainder to buf[0] and shrink blen.
    }
  }
  ```
- **Line-buffer policy.** `buf` is `CLAUDE_LINE_MAX`. Splitter scans for `'\n'`,
  dispatches each complete line, compacts the remainder to the front. If a single
  line reaches `CLAUDE_LINE_MAX` with no newline (oversized): dispatch the
  truncated line with a truncation marker, log a warning, and reset `blen`. On
  **natural EOF** (`eof`): best-effort dispatch of any non-empty remainder (a
  provider that omits a trailing newline does not lose its last event). On
  **cancel**: discard the remainder.
- **Remove** the `if (!cctx->conn_alive) { kill … }` block (`:1353-1367`).
- **Single reap** (replaces both the old happy-path and disconnect-path reaps;
  the owning worker is the only reaper). **SIGCHLD disposition:** worker threads
  block `SIGCHLD` (`pthread_sigmask`) around fork/wait so no stray handler
  auto-reaps the child; `waitpid` is the sole reaper. Error handling makes
  `reaped` a fact with exactly one writer:
  ```
  if (!e->reaped) {
    if (cancelled) { kill(pid, SIGTERM); bounded_wait_or_SIGKILL(pid); }
    int w; do { w = waitpid(pid, &status, 0); } while (w < 0 && errno == EINTR);
    // w<0 && ECHILD -> already reaped elsewhere; treat as reaped
    turn_registry_mark_reaped(e);   // sets e->reaped = 1 (single writer: this worker)
  }
  ```
  `bounded_wait_or_SIGKILL` reuses the existing 50×100ms wait then `SIGKILL`.
- `conn_alive` is no longer read in this function except inside `stream_event`
  (Step 2).

## Step 6 — In-process agent path (WP-4)

**`src/posix/server_compute.c:544` `chat_stream_worker_agent` (and `_primary_session` `:638`)**
- **Integration point (confirmed):** `agent_run_with_tools` (`agent_exec.h:46`)
  has **no** interrupt parameter, so cancellation is threaded via a new
  thread-local mirroring the existing request-context setters
  (`agent_set_request_session` etc., `agent_config.h:48-68`): add
  `void agent_set_request_cancel(_Atomic int *flag);`. The agent worker caches
  its `turn_entry_t *e` (returned by `turn_registry_publish`, passed in by
  `chat_stream_worker_pooled`) and calls `agent_set_request_cancel(&e->cancel)`
  before `agent_run_with_tools`, then `agent_set_request_cancel(NULL)` after
  (same pattern/placement as `agent_set_request_vault_principal` at
  `server_compute_async.c:476-478`).
- The agent loop reads the flag at safe points (between steps / before each tool
  call) and propagates it into the streaming callback's non-zero-return-aborts
  contract (`agent_exec.h:267`) and `agent_shell`'s `volatile int *interrupted`
  (`agent_shell.h:36`). A WP-6 test cancels mid-tool-call.

## Step 7 — Triggers (WP-4)

**`src/server/server_session.c:37` `handle_session_close`**
- Before `presence_session_close(sid)`: `turn_registry_cancel(sid);`

**`src/server/server.c:1706` `server_shutdown`** (runs in normal context after the
signal handler set `g_ctx.running=0`; async-signal-safe by construction)
- **Ordering (fixes use-after-close race):**
  1. `turn_registry_cancel_all()` — set every turn's atomic cancel flag.
  2. `server_compute_async_drain()` (`:1719`) — wait for workers to observe the
     flag and exit. The `_Atomic` flag + the drain's existing condvar provide the
     visibility barrier; workers exit within one poll tick + reap bound.
  3. **Only then** close every active presence session.
  Presence sessions are **not** closed before the drain, so a still-running
  worker cannot emit onto a torn-down ring. (`presence_publish` is already a
  no-op for an unknown session — `presence.h` — so even a late emit during the
  window is safe, but the ordering removes the window entirely.)

**`src/server/server.c` method table (~`:1163`) + new handler**
- Register `{"chat.graceful_cancel", handle_chat_graceful_cancel}`.
- `handle_chat_graceful_cancel(ctx, conn, req)`: read `aimee_session_id`; resolve
  caller principal from `conn` identity; `presence_session_owner(sid, owner)`;
  reject (`server_send_error` "forbidden") if principal != owner; else
  `turn_registry_cancel(sid)` and OK. (Fixes the gateway `/stop`, currently a
  no-op.)

## Step 8 — Webchat backend (WP-5)

**`webchat/chat.go`**
- `handleChatSend`: stop passing `r.Context()` as the turn lifeline. Start the
  turn server-side and return **202** with `{session_id, turn_id}`.
- **No missed-first-event (verified ordering).** Server-side, the presence
  session + ring are created and `turn_started` is published **before** the async
  worker is dispatched: `handle_chat_send_stream` →
  `presence_turn_acquire`/`presence_emit_turn_started` happen synchronously on
  the request thread (`server_compute_async.c:704,450-451`) **before**
  `chat_stream_dispatch` returns and the 202 is sent. Even so, the client never
  depends on timing: it subscribes to the events stream from **cursor 0**, and
  the ring replays `turn_started` + all deltas from the beginning. The 202 may
  thus return before, during, or after any event without loss. An end-to-end
  test asserts a client that subscribes only *after* the 202 still receives
  `turn_started`.
- New `handleChatEvents` proxy: `GET /api/chat/events?sid=&cursor=` →
  `GET /v1/sessions/{id}/events?cursor=` over the UDS, streamed to the browser
  as SSE; forwards the `gap` marker + server tail.
- Move `touchChatSession` to fire when the proxied stream yields `turn_started`
  (so cwd/title persist even if the browser left mid-turn).

## Step 9 — SPA (WP-5)

**`frontend/src/pages/Chat.tsx`** (+ events client)
- After `POST /api/chat/send` (202), render **only** from
  `GET /api/chat/events?sid=&cursor=`; remove the direct streaming POST as a
  render source.
- Persist the last-consumed cursor per session (localStorage cache + the existing
  per-user session metadata used by `SESSION_PERSISTENCE.md`).
- On reconnect: subscribe from the saved cursor. On a `gap` response, fetch and
  render the persisted final result (history) for any completed turn, then
  resubscribe at the returned oldest-live cursor.

## Step 10 — Tests + build (WP-6)

**C (`src/tests/`):**
- Extend `test_presence.c`: single-attach turn publishes deltas; `conn_alive=0`
  mid-turn → completion + full sequence incl. `usage` + `turn_done` on ring +
  persisted (via `g_chat_dispatch_override` + stub provider).
- New `test_turn_registry.c`:
  - `publish` returns an entry; a second `publish` for the same session →
    **collision returns NULL** (Step 4).
  - `cancel` sets the atomic flag; `find` reflects it; `cancel_all` flags all.
  - **reaped-flag single-`waitpid`** under cancel-vs-natural-exit race (no
    double-signal / double-reap).
  - **cross-principal `graceful_cancel` rejection** (owner != caller).
  - `sweep_dead` reaps an entry whose owner thread is gone.
- CLI loop edge cases: no-output provider cancel breaks loop within a bound;
  oversized line (no newline) truncates+marks; provider without trailing newline
  on EOF still delivers its last event; cancel discards the remainder.
- **Agent path:** cancel mid-tool-call aborts the in-process agent run (Step 6).
- **Shutdown drain bound** with a long detached turn (cancel-all → drain →
  close).
- Coalescing: delta-heavy turn stays under ring-publish bound; a `tool_call`/
  `usage` event never reorders ahead of preceding text.
- Register new tests in the test build (`CMakeLists.txt` / `src/tests` harness).

**Go (`webchat/`):**
- `chat_test.go`: reconnect replays from cursor; observes `turn_done` for a turn
  completed while detached; `gap` → history fallback (fake events endpoint);
  **202-then-events flow** — a client subscribing only after the 202 still
  receives `turn_started` from cursor 0.

**Build & verify:**
- `cmake --build <build> --target aimee-server` and the unit-test target; run the
  presence + turn_registry tests.
- `cd webchat && go test ./...`.
- Manual: webchat turn, close tab mid-turn, reopen → completed with full output.

## Files touched (summary)

- `src/headers/presence.h`, `src/server/presence.c` (owner accessor)
- `src/headers/turn_registry.h`, `src/server/turn_registry.c` (new)
- `src/posix/server_compute.c` (stream_event(_usage), CLI loop, agent path)
- `src/server/server_compute_async.c` (always-publish, registry publish/clear)
- `src/server/server_session.c` (cancel on close)
- `src/server/server.c` (shutdown cancel-all; graceful_cancel handler+route)
- `webchat/chat.go` (202 + events proxy)
- `frontend/src/pages/Chat.tsx` (events render source + cursor + gap)
- `src/tests/test_presence.c`, `src/tests/test_turn_registry.c`,
  `webchat/chat_test.go`, build wiring

## Roundtable P1 resolutions (traceability)

- *Step 2 reentrancy/reorder* → whole emit serialized under existing
  `cctx->write_mutex`; ring publish + buffer mutation inside it.
- *Step 4 session-only keying / one-turn invariant* → invariant documented
  (presence turn-lock); `publish` returns NULL on collision; caller rejects.
- *Step 4 publish collision* → NULL return, hard error, no silent overwrite.
- *Step 4 cancel_all/sweep_dead lock discipline* → registry mutex is a leaf; no
  presence/compute lock held across registry calls; `sweep_dead` caller +
  dead-predicate defined.
- *Step 5 cancel memory ordering/race* → `_Atomic` flag, seq_cst; re-check before
  `read()` and on EAGAIN/EINTR.
- *Step 5 SIGCHLD/waitpid* → block SIGCHLD in workers; EINTR retry; ECHILD →
  treat reaped; single writer of `reaped`.
- *Step 5 line buffer* → splitter/compaction, oversized policy, EOF-remainder
  dispatch, cancel-discard specified + tests.
- *Step 6 interrupt wiring* → confirmed no param; add `agent_set_request_cancel`
  thread-local; worker caches `turn_entry_t*`.
- *Step 7 shutdown ordering* → cancel-all → drain → close (not before); barrier
  via `_Atomic` + drain condvar; `presence_publish` no-op-on-unknown pinned.
- *Step 8 202 timing* → turn_started published before dispatch; client replays
  from cursor 0; e2e test.
- *Step 10 coverage* → added collision, agent cancel, line-buffer edges, 202-flow,
  shutdown drain, coalescing-order, graceful_cancel authz tests.
