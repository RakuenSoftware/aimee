# aimee-server

This directory is the ownership boundary and implementation home for
`aimee-server`, the persistent local hub that every thin client talks to.
For the system-level picture see [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md);
for the code-level module map and RPC catalog see the
[Technical Reference](../README.md).

## What aimee-server is

`aimee-server` is a single C11 process that owns **DB1** (local SQLite), runs the
bounded compute pool, routes delegates, serves the lifecycle hooks, and exposes
the public HTTP `/v1` surface (its only client transport). The thin clients
(`aimee`, `aimee-webchat`) hold no database; they reach this process over `/v1`
(local `aimee-http.sock` or remote TCP) and forward typed requests. The expensive
state (open DB handles, a
warm KB connection, the worker pool) lives here, in a warm long-running process,
which is what keeps CLI startup under 10 ms and a pre-tool hook check around 1 ms.

It links `-lsqlite3` (plus `-lssl`/`-lcrypto`/`-lpam`) and is built with
`-DAIMEE_DB2_DISABLED`: it **never** links libpq and holds no DB2 SQL. All
knowledge operations go through the typed **KB client** (`kb_client*.c`) to
`aimee-kb`. The storage boundary is compile-enforced (`make check-linking`,
`readelf` symbol checks).

## Scaling contract: one server per user

**`aimee-server` is strictly 1:1 with a user.** This is a design invariant, not a
deployment choice:

- It owns **local, same-user runtime state** in DB1: sessions, working memory,
  conversation windows, checkpoints, durable delegate jobs, secrets. That state is
  private to one OS user on one host.
- It authenticates callers by **`SO_PEERCRED`** (the connecting process's UID).
  The trust model is "same Unix user = same principal"; there is no notion of a
  remote tenant.
- It is therefore **single-tenant and local by construction**. You do not shard
  it, replicate it, or put it behind a load balancer. Each developer (each OS
  login) runs exactly one `aimee-server` on their own machine, normally as a
  systemd user unit or launchd agent.

Scale-out happens **below** the server, in the shared tier: many independent
`aimee-server` instances (= many users) point at one shared `aimee-kb`/Postgres
deployment. The server is the per-user front; the KB is the many-user back. See
[`src/kb/README.md`](../kb/README.md) for that side of the boundary.

## Internals at a glance

| Concern | Where | Notes |
|---------|-------|-------|
| Event loop | `server.c` (`server_run`) | Single epoll thread: read, parse, dispatch, and **never blocks**. Per-connection buffered I/O; `EPOLLOUT` added only while a write is pending. |
| Entry / signals | `server_main.c` | Startup handshake, SIGTERM/SIGINT graceful drain, unclean-exit forensics to DB1. |
| Auth | `server_auth.c` | Capability lookup + a 16-bit capability mask declared per method (unknown methods default to deny). The local `/v1` UDS is filesystem-trusted (full surface); the optional TCP listener is bearer-gated (`server_http_authorize`, constant-time compare via `server_ct_equal`). |
| Sessions | `server_session.c` | Session create/list/get/close, per-session worktrees and state. |
| State handlers | `server_state.c` | Fast handlers: memory, index, rules, working memory, dashboard (routed to tier-owned APIs). |
| Compute pool | `server_compute*.c`, `compute_pool.c`, `server_jobs_aux.c` | Bounded worker threads for anything that blocks (DB, network, subprocess, delegate inference); a compute-budget gate serializes heavyweight inference. Durable/background delegate jobs survive client disconnect. |
| KB client | `kb_client*.c` | Typed RPC wrappers (memory, index, docs, agent, dashboard, roadmap, notes, curiosity, status) to `aimee-kb` over a Unix socket or HTTP (`AIMEE_KB_API_URL`, port 8741). The server holds **no** DB2 SQL. |
| Delegation | `agent_runtime.c`, `agent_loop.c`, `agent_policy.c`, `agent_tools.c`, `delegate_routing.c`, `delegate_openai.c`, `http_retry.c` | Cost-based route selection, tool-use loop (bash/read/write), provider HTTP shaping, retry/fallback. |
| HTTP `/v1` | `server_http.c`, `server_api.c` | OpenAI-compatible inference, run management, read/control surfaces. UDS callers auth by peer UID; TCP callers present a bearer token (optionally `scope:`-prefixed). Contract: [`api/openapi-server-v1.yaml`](../../api/openapi-server-v1.yaml). |

## Concurrency model

- **Event-loop thread** owns socket I/O and dispatch and never blocks.
- **Compute pool** runs blocking handlers; sizes are tunable in `aimee.yaml`
  (`compute_threads` / `worker_threads` / `background_threads` / `session_threads`).
- **Compute-budget gate** (`server_compute_budget_acquire/release`) bounds
  concurrent heavyweight inference so a burst of delegates cannot exhaust the host.

These knobs scale a single user's server *vertically* (more in-flight delegates,
more parallel tool calls). They do **not** make the server multi-user; that
remains the KB tier's job.

## Boundary rules

Server code talks to `aimee-kb` only through the KB client contract; KB code must
not include server internals. New server-only implementation files belong under
this directory. The build gates (`make check-linking`,
`make module-boundary-check`, `scripts/check_tier_deps.sh`) fail the build if a
`db2_*`/`PQ*` symbol appears here, or if code outside a tier reaches a tier's
storage handles directly.
