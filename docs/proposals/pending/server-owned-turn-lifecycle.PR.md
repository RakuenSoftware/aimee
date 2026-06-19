# Server-owned turn lifecycle (Phase 1): a turn outlives the connection

## Why

For aimee to act as an **autonomous agent**, a turn and session must be owned by
the server, not by a client connection. Today closing the webchat tab (or any
client drop) **aborts** the in-flight turn: the CLI worker gates its read loop
on `conn_alive` and SIGKILLs the provider subprocess on disconnect. This PR
makes a turn run to completion server-side regardless of who is watching, with
the full turn stream on the presence event ring for live + reconnect replay.

This is **Phase 1** of the multi-channel effort (Telegram/Discord surfaces,
agent-initiated outreach are later phases).

## What changed (server-side core)

- **The presence ring is the unconditional sink.** `stream_event` /
  `stream_event_usage` publish the full turn stream (text/thinking/tool/usage +
  boundaries) to the ring first, then best-effort to the socket; `conn_alive`
  now gates **only** the socket write. The whole emit is serialized under the
  existing per-`cctx` `write_mutex`. Text deltas are coalesced (≤50 ms / 2 KB),
  ordering-safe (non-text flushes pending text first).
- **Always mirror to the ring** for any presence-tracked session (dropped the
  `>1`-attachment gate).
- **New `turn_registry`** — per-turn atomic cancel flag + single-reaper child
  ownership, leaf-mutex lock discipline, NULL-on-collision, crash-backstop sweep.
- **CLI worker** rewritten to an `O_NONBLOCK` + `poll()` read loop (no `fgets`):
  a dropped connection no longer ends the loop or kills the child; the turn ends
  only on provider EOF or an explicit cancel; single reap with EINTR/ECHILD
  handling.
- **In-process agent path** cancels cooperatively via `agent_set_request_cancel`.
- **Triggers:** `session.close` and server shutdown cancel in-flight turns
  (cancel-all **before** the drain so it stays bounded); new
  `chat.graceful_cancel` handler with owner-authz — also fixes the gateway
  `/stop`, previously a no-op.
- `presence_session_owner` accessor; `test_turn_registry` unit test.

## Process (roundtable-gated)

Authored and reviewed through aimee's own multi-agent roundtable
(`/v1/delegate/roundtable`, review mode, minimax + mistral + mimo panel):

| Stage | Rounds | Gating findings |
|------|--------|-----------------|
| Design proposal | 4 | 25 → 7 → 1 → **0 (approved)** |
| Implementation plan | 2 | 11 → **0 (approved)** |
| Implementation diff | 3 | 5 → 7 → 0 (weak panel) |
| Implementation diff (re-review) | 2 | 5 → **0 (approved)** |

A mid-review reliability bug was caught and fixed: one panelist
(`mistral-medium`) was returning thin 1–10 s reviews. Upgrading that seat to
`mistral-large` produced a substantive 3-model panel, which surfaced 5 real
issues the thin panel had passed (unwired racy `sweep_dead`, `waitpid` ECHILD
misclassification, a cancel-registration ordering gap, graceful_cancel audit).
Those were fixed and the diff re-approved with `participants_failed: 0`.

Design + plan: `docs/proposals/pending/server-owned-turn-lifecycle{,.plan}.md`.

## Scope / follow-ups

- **Webchat `202 + events` and SPA reconnect/replay (WP-5)** are the documented
  fast-follow — the plan states the server is correct and testable on its own
  after the trigger wiring. This PR is the server-side core.
- Finer-grained mid-call interruption of the in-process agent path (the CLI
  path — the default — is fully interruptible).
- Wire `turn_registry_sweep_dead` to the periodic compute-thread bookkeeping
  (crash backstop; not on the primary path).

## Testing

`test_turn_registry` covers publish/collision/cancel/authz/cancel-all/reaped.
⚠️ **Not built locally** — this workspace has no C toolchain; CI compiles and
runs the suite.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
