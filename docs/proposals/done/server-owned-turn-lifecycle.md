# Server-owned turn lifecycle: a turn outlives the connection that started it

Status: done

## Problem

A chat turn's execution is today bound to the client connection that started
it. When that connection drops — the user closes the webchat browser tab, the
laptop sleeps, a phone loses signal — the in-flight turn is **aborted**, not
merely detached:

- The CLI-backed chat worker gates its provider read loop on
  `cctx->conn_alive` (`src/posix/server_compute.c:1233`) and, on a dropped
  connection, **SIGTERM/SIGKILLs the provider subprocess**
  (`src/posix/server_compute.c:1353-1367`). The turn ends mid-flight.
- Webchat ties the upstream turn to the browser's HTTP request context
  (`webchat/chat.go:120`, `chatStreamHTTP(r.Context(), …)`), so a browser exit
  cancels the unix-socket call into aimee-server, which is what trips
  `conn_alive` in the first place.

This contradicts aimee's goal of being an **autonomous agent**. An autonomous
agent must keep working — and be able to reach out to a human mid-run —
regardless of whether any client is watching. A connection should be a *window*
onto a running session, not the *thread of life* for it.

The durable substrate to fix this already exists (unified-presence; see
Background). The turn execution path simply does not yet use it as its source of
truth. This proposal closes that gap. It is **Phase 1** of a larger
multi-channel effort (Telegram/Discord surfaces, agent-initiated outreach);
those later phases are out of scope here but motivate the design.

## Goal

A turn runs to completion on the server once started, and its full event stream
is durably observable, **independent of any client connection**:

1. Closing/dropping the originating connection **detaches**; it never cancels
   the turn or kills the provider.
2. The complete turn stream is published to the per-session presence-event ring,
   so it can be **replayed** to a reconnecting client and observed live by any
   other attached surface.
3. A client reconnecting mid-turn resumes from a cursor; on turn completion it
   sees the final result even if *no* client was attached when the turn
   finished.
4. The turn result is persisted to session history exactly as today.
5. Turns remain **bounded and cancellable** without a connection: a hung
   provider, a closed session, or server shutdown can stop a detached turn
   promptly.

### Source-of-truth model (resolves R1: ring-vs-history, crash recovery)

- **Session history (durable, DB/provider session keyed by session id) is the
  canonical source of truth** for a *completed* turn's full output.
- **The presence-event ring is a volatile, in-process, bounded buffer** for
  *live* streaming and short-window *tail* replay to reconnecting clients.
- On **server crash** mid-turn the volatile ring is lost and the turn is **not**
  resumed (Phase 1 has no durable per-turn write-ahead log). Stated as an
  explicit limitation; a durable turn log is a candidate for a later phase.

### Non-goals (this phase)

- Agent-initiated outreach (`send_message`/`ask_user` made real) — Phase 2.
- Cross-process delivery to Telegram/Discord via the gateway — Phase 3.
- A Discord adapter; webchat fully symmetric with gateway surfaces — Phase 4.
- A durable per-turn event log surviving server crash (see above).
- Changing turn *arbitration* (single-turn lock + FIFO queue) — reused unchanged.

## Background: what already exists (reuse, don't rebuild)

`src/server/presence.c` + `src/headers/presence.h` implement the
unified-presence model: a **session** is durable state, an **attachment** is a
disposable connection (closing one does not destroy the session), a per-session
**event ring** with `presence_publish`/`presence_wait` (cursor-based, mirrors
`openai_runs_store`), and `GET /v1/sessions/{id}/events`
(`server_http_routes.inc:1586`) streaming it live + replay-from-cursor. The turn
worker `chat_stream_worker_pooled` (`src/server/server_compute_async.c:402`)
already brackets every turn with a presence turn-lock and
`turn_started`/`turn_done`.

### Code audit (verifying R1 correctness claims before design)

- **Event funnel:** every turn event is emitted via `stream_event`
  (`src/posix/server_compute.c:78`) — `turn_start`, `text`, `thinking`,
  `session`, tool-call phases (`:541`), `turn_end`, `done` — across all worker
  variants (agent `:544`, primary-session `:638`, CLI/claude `:1233+`, compact
  paths). **Exception:** token usage is emitted by a *separate* function
  `stream_event_usage` (`:123`), which does **not** currently mirror to the ring.
  WP-1 must cover both functions.
- **`turn_done` independence:** `chat_stream_worker_pooled` calls
  `presence_turn_release` / `presence_emit_turn_done`
  (`server_compute_async.c:480-482`) **unconditionally after the worker
  returns**, using `lock_session`/`lock_turn`/`sid`/`turn_id` copied into locals
  *before* the worker ran (`:422-431`) — valid even though sub-workers free
  `cctx`. So `turn_done` already reaches the ring independent of `conn_alive`,
  provided the worker *returns* (which WP-2 guarantees).
- **Persistence independence:** the only `conn_alive` reads on the worker path
  are in `stream_event`/`stream_event_usage` (best-effort writes) and the
  read-loop guard `:1233` (removed by WP-2). No history-persistence call site
  consults `conn_alive`; history is written by the provider/CLI session keyed by
  session id. Persistence is therefore already connection-independent.
- **Cancellation today:** there is **no** `chat.graceful_cancel` handler in the
  method table (`server.c`) — the gateway `/stop` is currently a no-op. The CLI
  worker's subprocess pid is a local (`:1113`). The *only* existing interrupt is
  the `conn_alive` disconnect-kill. Removing it (WP-2) therefore requires a real
  cancellation primitive (WP-4) — they must ship together.
- **Shutdown drain:** `server_shutdown` (`server.c:1706`) calls
  `server_compute_async_drain()` → `chat_thread_drain()` which **blocks until
  all chat threads exit** (`:1719`). A long detached turn would hang shutdown
  unless cancelled first (WP-4).
- **Existing interrupt hooks to reuse:** the streaming callback aborts on
  non-zero return (`agent_exec.h:267`) and `agent_shell` checks a
  `volatile int *interrupted` each iteration (`agent_shell.h:36`). WP-4 drives
  these from a per-turn cancel flag.

## Design

### WP-1 — The presence ring is always the full, faithful turn stream

Today the worker mirrors deltas to the ring **only when a second surface is
attached** (`server_compute_async.c:440`, `presence_attachment_count(...) > 1`).
The originating connection is otherwise the only place the stream exists.

Changes:
- **Always publish** on any presence-tracked session: set
  `presence_emit_deltas` whenever `delta_session[0]` is set; drop the `> 1` gate.
- **Cover all event classes,** including usage: both `stream_event` (`:78`) and
  `stream_event_usage` (`:123`) publish to the ring.
- **Restructure the `conn_alive` guard so it never gates the ring (critical).**
  Today `stream_event` early-returns at the top when `!conn_alive` (`:80`) —
  *before* the ring publish at `:107-116`. Left as-is, a dead connection would
  skip the ring publish and defeat this entire proposal. The functions are
  reordered so the **ring publish runs unconditionally**, and the `conn_alive`
  check is narrowed to wrap **only the direct-to-socket `write_all` calls**
  (`:94-96`, and the equivalent in `stream_event_usage` `:140-142`). Concretely:
  build the event JSON, publish to the ring (coalesced per the rule below),
  then — only if `conn_alive` — attempt the socket write, flipping `conn_alive`
  to 0 on write failure as today. The ring becomes the unconditional sink; the
  socket is the best-effort, connection-scoped sink.
- **Ring event schema** (additive; unknown kinds ignored by older consumers).
  Turn-boundary events stay `turn_started` / `turn_done`. Content events are
  published as `turn_delta` with payload:
  `{"turn_id": "<id>", "kind": "text"|"thinking"|"tool_call.<phase>"|"usage", …}`
  where `text`/`thinking` carry `"content"`, `tool_call.<phase>` carries
  `"name"`, `usage` carries `{"in","out","cost","usage_kind"}`. Ordering is the
  ring's FIFO/cursor order. CLI TUI and webchat SPA consumers are updated to
  render the new kinds; unknown kinds are skipped, so the change is
  backward-compatible.
- **Text-delta coalescing (ordering-safe):** coalesce *text* deltas into the
  ring on a ~50ms / N-char window. **Only text is batched.** Any non-text event
  (`thinking`, `tool_call.*`, `usage`, `turn_done`) **first flushes the pending
  text batch, then publishes immediately** — so the ring's cursor order never
  reorders a tool call or usage relative to the text around it. The live
  direct-to-connection write path is unchanged (still per-delta); only the ring
  publish is coalesced. This caps how fast `PRESENCE_EVENT_RING` fills,
  mitigating the retention risk.

After WP-1, `GET /v1/sessions/{id}/events` is a faithful, cursor-ordered,
replayable mirror of every turn on every session.

### WP-2 — `conn_alive` gates one connection's writes, never turn execution (ships with WP-4)

`conn_alive` must mean "this one connection can still receive best-effort
writes," not "keep running the turn." Two edits in `src/posix/server_compute.c`:

1. **CLI worker read loop** (`:1233`): drop the `conn_alive` guard. To keep the
   loop interruptible (a hung provider must not block forever — see WP-4), set
   `O_NONBLOCK` on the provider pipe fd and replace blocking `fgets` with a
   `poll()` on the fd with a short timeout (~200ms); each wakeup checks the
   per-turn cancel flag (WP-4), then drains available bytes into a line buffer,
   handling `EAGAIN`/partial reads (accumulate until newline; never block on a
   half-line; cap the line buffer at `CLAUDE_LINE_MAX` and flush an oversized
   line rather than growing unbounded). Per WP-1's restructure, `stream_event`'s
   `conn_alive` guard now wraps only the socket write, so writes to a dead socket
   are skipped while the loop, **ring publishing**, and result accumulation
   continue. **Interruptibility contract:**
   on cancel the loop breaks within one poll tick; the provider subprocess is
   then expected to respond to `SIGTERM` within the bounded wait before
   `SIGKILL` (WP-4). A WP-6 test cancels a provider that emits no output at all
   (pure block), proving the poll loop — not provider output — is what makes the
   loop interruptible.
2. **Disconnect-triggered kill** (`:1353-1367`): removed. The subprocess is
   reaped on natural completion (happy path) or by the cancel primitive (WP-4).

The in-process agent path (`:544`) already runs `agent_run_with_tools` to
completion without consulting `conn_alive`; it inherits WP-1 ring publishing via
the funnel and WP-4 cancellation via the `interrupted` flag.

**WP-2 and WP-4 ship in a single PR.** Enforcement is behavioral, not textual: a
unit test (WP-6) asserts that (a) a turn with `conn_alive=0` still completes and
emits `turn_done`, and (b) session-close cancels a long turn within the drain
bound. Both must pass in the same change; WP-2's loop edit cannot land green
without WP-4's cancel path.

### WP-3 — Completion and persistence are connection-independent (verified)

Consequence of WP-1 + WP-2, confirmed by the audit above:
- `presence_turn_release` / `turn_done` run unconditionally after the worker
  returns (`server_compute_async.c:480-482`) → terminal event reaches the ring.
- No persistence call site consults `conn_alive` → the final result is written
  to session history regardless of connection state.

Captured as acceptance criteria + tests (WP-6), not new code.

### WP-4 — Cancellation primitive + bounded lifetime (ships with WP-2)

Replace the connection-as-leash with an explicit, connection-independent cancel
primitive.

- **Per-turn cancel registry:** a small fixed table keyed by session id holding
  `{ volatile sig_atomic_t cancel; pid_t child_pid; int reaped; }`. One mutex
  guards it; lookups are O(n) over a bounded table (mirrors the gateway session
  table sizing). **Lifecycle:** the entry is published *before* the worker is
  dispatched (so a cancel arriving immediately is not lost), and cleared only
  *after* the child is reaped and `turn_done` is emitted. **Lock hierarchy:**
  the registry mutex is a leaf — it is never held while acquiring a presence
  turn-lock or the compute locks (acquire order is always presence/compute →
  registry, released in reverse), so cancel-all during shutdown cannot deadlock
  against a worker. A cancel only *sets the flag and reads the pid* under the
  registry mutex; the actual signal/`waitpid` happens outside it, in the owning
  worker.
- **Single reap owner (no double-signal / double-waitpid):** the owning worker
  is the only caller of `kill`/`waitpid` for its child. The `reaped` flag guards
  it: `SIGTERM`/`SIGKILL` are sent only if `child_pid > 0 && !reaped`, and
  `waitpid` runs exactly once, after which `reaped=1`. The read loop checks the
  cancel flag *before* signalling; if the child already exited naturally
  (`poll` returns `POLLHUP`/EOF), the natural completion path reaps it and sets
  `reaped` — so a cancel racing a natural exit is a no-op, not a double-reap.
- **Propagation:**
  - CLI path: the `poll()`-based read loop (WP-2) checks `cancel` each wakeup;
    on set it breaks into the single-owner reap sequence above.
  - In-process agent path: the per-turn `cancel` flag is threaded to
    `agent_run_with_tools` via the existing `volatile int *interrupted` /
    streaming-callback-abort hooks (`agent_shell.h:36`, `agent_exec.h:267`),
    checked at safe points between steps/tool calls.
- **Triggers:**
  - `handle_session_close` (`server_session.c`) sets `cancel` for that session
    before `presence_session_close`.
  - `server_shutdown` sets `cancel` for **every** active turn and closes every
    active presence session **before** `server_compute_async_drain()`
    (`server.c:1719`), so the drain is bounded. **Signal-driven shutdown is
    async-signal-safe:** the signal handler only sets a `volatile sig_atomic_t
    g_stop` flag and returns (the existing pattern); the main loop observes it
    and performs cancel-all + close-all-presence + drain in normal context. No
    non-async-signal-safe work (mutexes, `presence_session_close`, drain) runs
    inside the handler.
  - `chat.graceful_cancel` is **registered** (new handler) to set `cancel` by
    session id. **Authz:** the handler verifies the caller's principal/owner
    matches the target session's presence owner and rejects a mismatch — a
    client cannot cancel another client's turn via a forged session id. Presence
    already records `owner` per session (`presence.c:53`); this adds a small
    `presence_session_owner(sid, out)` accessor. The webchat and gateway surfaces
    additionally bind session id to the authenticated caller before forwarding
    (webchat via its login session, gateway via pairing). A WP-6 test asserts
    cross-principal cancel is rejected.
- **Worker-crash backstop:** the single-reap owner assumes the worker reaches
  its reap. If a worker crashes before reaping, its child reparents to init and
  the registry entry would leak; a periodic sweep (reusing the existing
  compute-thread bookkeeping) clears registry entries whose owning thread is
  gone. Defense-in-depth, not the primary path.
- **Intrinsic bounds remain:** turns are also capped by `AGENT_DEFAULT_MAX_TOKENS`
  and provider deadlines, so a detached turn with no audience terminates on its
  own even absent an explicit cancel.

### WP-5 — Webchat: disconnect detaches, reconnect replays (full protocol)

Webchat stops treating the turn POST as the turn's lifeline; the browser renders
**only** from the durable events stream.

- **Two-step protocol.** `POST /api/chat/send` accepts the turn, starts (or
  rejoins) it server-side, and returns **`202 Accepted` with
  `{session_id, turn_id}`** immediately — it no longer holds the request open
  for the turn's duration, and no longer passes `r.Context()` as the turn's
  lifeline. The SPA then renders from `GET /v1/sessions/{id}/events?cursor=<n>`
  (proxied by webchat) as the **single** render source. The old direct
  `chatStreamHTTP(r.Context(), …)` render path (`webchat/chat.go:120`) is
  **removed** as a render source.
- **Synchronous ring creation (no lazy-init race).** The presence session and
  its event ring are created **synchronously inside `POST /api/chat/send`,
  before the worker is dispatched**, and `turn_started` is published as the
  ring's first event. The ring is therefore never lazily created on first GET —
  a `turn_started` and all early deltas are guaranteed retained from cursor 0,
  even if the client's GET subscription arrives late or never. (`launch.run` /
  session bootstrap already mint the session id; this only guarantees the
  in-process presence+ring exist before any worker event can fire.)
- **No double-delivery.** Because the browser renders only from the cursor-keyed
  ring stream, there is one source; replays are idempotent by cursor. The live
  case and the reconnect case are the same code path (subscribe at a cursor).
- **Cursor persistence.** The SPA persists the last-consumed ring cursor per
  session in the same per-user store as `SESSION_PERSISTENCE.md` (plus a fast
  localStorage cache). On reconnect it calls the events endpoint with that
  cursor; the server replays buffered events after it, then continues live.
- **Gap fallback (resolves AC5/retention).** If the saved cursor is older than
  the ring's retained window, the events endpoint returns a `gap` marker **plus
  the server-side tail of currently-retained events and their oldest live
  cursor**, so recovery does not depend solely on the client's stale cursor. The
  SPA renders the **persisted final result from history** for any completed turn
  (durable source of truth), then resubscribes at the returned oldest-live
  cursor for the live tail. Thus a long turn whose early deltas were evicted
  still shows correct final output.
- **POST/GET race.** None: the ring buffers from turn start, so a GET
  subscription established *after* the POST returns still replays every event
  from its cursor (0 for a fresh tab). `turn_id` from the 202 lets the SPA match
  the stream to the turn it just started.
- **Metadata.** `touchChatSession` (cwd/title) moves to fire on the
  `turn_started` event from the stream, so it records even when the browser left
  mid-turn.

The CLI already demonstrates the attach + event-consume shape
(`cli_chat_presence_attach`, `src/cli_tui.c:474`).

### WP-6 — Tests

- **C unit:** turn with `conn_alive=0` mid-stream (a) runs to completion,
  (b) publishes the full event sequence incl. `usage` and `turn_done` to the
  ring, (c) persists its result to history. Uses the `g_chat_dispatch_override`
  seam + a stub provider; asserts via `presence_wait`. Extends
  `src/tests/test_presence.c`.
- **C unit:** single-attachment turn publishes deltas to the ring (WP-1
  regression guard — previously gated off).
- **C unit:** `session.close` cancels an in-flight turn; the subprocess is
  reaped within the drain bound (WP-4).
- **C unit:** server-shutdown path cancels all active turns then drains within a
  bounded time with a long detached turn running (WP-4).
- **C unit:** `chat.graceful_cancel` cancels by session id; a cancel from a
  different principal than the session owner is **rejected** (WP-4 authz).
- **C unit:** cancel of a provider that emits **no output at all** still breaks
  the read loop promptly (WP-2 poll-driven interruptibility, not output-driven).
- **C unit:** a cancel racing a natural subprocess exit produces exactly one
  `waitpid` and no double-signal (WP-4 single-reap `reaped` flag).
- **C unit:** ring-fill coalescing keeps per-turn ring publishes under a bound
  for a delta-heavy turn, and a `tool_call`/`usage` event never reorders ahead
  of preceding text (WP-1 perf + ordering guard).
- **Go:** webchat reconnect replays buffered events from a cursor, observes
  `turn_done` for a turn completed while detached, and falls back to the
  persisted result on a `gap` (table test against a fake events endpoint).
- **Manual/verify:** start a turn in webchat, close the tab mid-turn, reopen →
  the turn shows completed with full output.

## Roundtable R2 resolutions (traceability)

- *WP-4 signal-handler async-unsafety* → handler only sets `g_stop`; main loop
  does cancel-all/close-all/drain in normal context.
- *WP-4 graceful_cancel authz* → handler verifies caller principal == session
  owner; surfaces bind session→caller; cross-principal cancel test.
- *WP-4 registry lock ordering / lifecycle* → registry mutex is a leaf
  (presence/compute → registry order); entry published before dispatch, cleared
  after reap + `turn_done`.
- *WP-4 double-signal / double-waitpid race* → `reaped` flag; single reap owner;
  cancel racing natural exit is a no-op; test added.
- *WP-5 ring creation timing* → presence+ring created synchronously in POST
  before worker dispatch, `turn_started` first event.
- *WP-5 gap recovery beyond client cursor* → gap response carries server-side
  tail + oldest-live cursor.
- *WP-1 coalescing reorder* → only text batched; non-text events flush text then
  publish immediately; ordering test.
- *WP-2 partial reads* → `O_NONBLOCK` + `EAGAIN` handling; no-output cancel test;
  documented SIGTERM-responsiveness contract.

## Roundtable R1 resolutions (traceability)

- *AC5 vs ring retention contradiction* → AC5 weakened to "tail + `turn_done` +
  persisted result"; durable history is the source of truth; WP-5 gap fallback.
- *WP-1 stream_event coverage / agent path* → audited; `stream_event_usage` added
  to WP-1; funnel + always-on `presence_emit_deltas` confirmed.
- *WP-2/WP-4 sequencing not enforced* → merged into one PR; behavioral test gate.
- *WP-2 hung-fgets, no interrupt* → `poll()` loop + per-turn cancel flag (WP-4).
- *WP-3 turn_done reachability* → audited unconditional at `:480-482`; AC + test.
- *WP-2 persistence independence* → audited; no conn_alive-gated persistence;
  test with `conn_alive=0`.
- *WP-4 cancel primitive undefined* → per-turn cancel registry + propagation +
  single reap owner + triggers (session-close/shutdown/graceful_cancel) defined.
- *WP-4 shutdown closes every session incl. signal path* → cancel-all + close-all
  before drain, routed through the signal handler.
- *WP-1 always-publish overhead* → text-delta coalescing folded into WP-1 + perf
  test.
- *WP-1/WP-5 ring event schema* → explicit `turn_delta` kind schema, additive.
- *WP-5 cursor storage / idempotency / double-delivery* → cursor in per-user
  store, single cursor-keyed render source, gap fallback.
- *WP-5 webchat request protocol* → 202 + `{session_id, turn_id}`, GET render
  source, POST/GET race addressed.
- *Ring vs history canonical source / crash recovery* → source-of-truth model
  section; crash = turn lost (explicit Phase-1 limitation).

## Risks

- **Ring-fill vs long turns:** coalescing (WP-1) bounds fill; gap fallback (WP-5)
  guarantees correct final output from history even if early deltas evict.
- **Detached turns consume compute with no audience:** bounded by token/deadline
  caps, session-close, shutdown cancel, and `graceful_cancel`. A cost/idle cap
  is a later-phase concern.
- **Cancel/reap races:** the cancel registry makes the worker the single owner
  of its child's reap; the racing disconnect-kill is removed. Tested under
  concurrent cancel + natural completion (WP-6).
- **Schema compatibility:** new ring `turn_delta` kinds are additive; older
  consumers skip unknown kinds. CLI TUI + SPA updated in the same change.

## Acceptance criteria

1. With a single webchat attachment, the full turn stream — incl. `usage` —
   appears on `GET /v1/sessions/{id}/events` (WP-1).
2. Marking the serving connection dead mid-turn does not abort the turn: it runs
   to completion, publishes `turn_done` to the ring, and persists its result
   (WP-2/WP-3) — verified by a C unit test with no live attachment at
   completion.
3. The provider subprocess is no longer killed on client disconnect; it is
   reaped on natural completion or by the cancel primitive
   (session-close/shutdown/graceful_cancel) (WP-2/WP-4).
4. A hung provider read cannot block the worker indefinitely; the `poll()` loop
   + cancel flag interrupts it (WP-2/WP-4).
5. Server shutdown cancels all active turns then drains within a bounded time
   even with a long detached turn running (WP-4).
6. Webchat: closing the tab mid-turn then reopening shows the completed turn with
   full output — from the ring tail within retention, otherwise from persisted
   history via the gap fallback (WP-5).
7. No regression to turn arbitration (single-turn lock + FIFO) or to
   session-history persistence.
