# `/v1` op-parity buildout: route→method map and wave plan

> Working tracker for **P1 / WP1.x / WP1.fin** of the aimee `/v1` hub-migration plan.
> Goal: **every NDJSON RPC method gets a dedicated `/v1` HTTP route** (or a
> documented, deliberate exclusion). The generic `POST /v1/rpc` passthrough is
> retired; this buildout gave each method a dedicated, self-documenting,
> OpenAPI-listed route.

## Mechanism (already landed: WP0.2, #2531)

The `/v1` surface is a declarative table in
`src/server/server_http_routes.inc`. Adding a dedicated route is:

1. **One table row.** For a single-response method that takes a **JSON-object
   body**, the row is `{"POST", "/v1/<family>/<verb>", NULL, RM_EXACT,
   "<family>.<verb>", 0, rh_dispatch_op}`. The `op` twin both dispatches the
   call (via the `server_dispatch` loopback bridge) **and** derives the required
   capability from `server_capability_for_method`, so `caps` stays `0`.
2. **One OpenAPI path** in `src/api/openapi-server-v1.yaml` (the conformance
   gate is spec→code: every documented path must be routed; `make
   server-api-conformance-check`).
3. Nothing else. No bespoke handler for the common case.

### When a row is **not** enough

- **Positional `args`-array methods** (e.g. `workspace.*`) need a small bespoke
  adapter; see `ws_dispatch_args` in the registry. Most CLI methods marshal a
  JSON **object** (named fields / `marshal_no_args`), so they take the trivial
  `rh_dispatch_op` row.
- **Streaming or foreground-blocking methods** must **not** use
  `rh_dispatch_op` (the buffered HTTP listener cannot block). These need a
  dedicated streaming handler (cf. `/v1/chat/stream`, `/v1/runs/{id}/events`) or
  are routed as async `runs`.

## Path / verb convention

- `family.verb` → `/v1/<family>/<verb>`.
- **GET** for genuinely no-arg reads (`marshal_no_args` methods: list/status/
  stats/board/…). `rh_dispatch_op` only reads the body, never the query string,
  so any read that needs params is **POST** with a JSON body.
- **POST** for all mutations and all param-bearing calls.
- One route per RPC method. Existing dashboard read-views (`GET /v1/agents`,
  `GET /v1/models`, `GET /v1/kb/status`, backed by `route_json_provider`) are a
  *different* curated surface and coexist with the per-method routes below.

## Deliberate exclusions (NOT given a dedicated route)

| Method(s) | Why |
|---|---|
| `hooks.pre`, `hooks.post`, `hooks.session_start` | Internal harness lifecycle hooks; now dedicated privileged routes at `/v1/hooks/*`, gated by `CAP_TOOL_EXECUTE`. |
| `runner.poll`, `runner.respond` | Already dedicated (`/v1/runner/*`, workspace detached reverse channel). |
| `primary.get/set/clear` | Already dedicated via `/v1/sessions/{id}/primary`. |
| `tool.execute` | Internal tool-execution path at `/v1/tools/execute` (workspace plane); gated by `CAP_TOOL_EXECUTE`, not a public verb. |

> **Inline-dispatch latency budget.** Dispatch-op routes run inline on the
> listener thread, so any routed method blocks other `/v1` callers for its
> duration. Sub-second and bounded-network methods (e.g. `agent.probe`,
> `provider.test`) are fine. **Long-running / LLM methods** (`kb.build`,
> `kb.ingest`, `kb.update`, `graph.sync_code`, `index.scan`, `memory.benchmark`,
> `curator.synthesize`, `rules.generate`, `eval.run`) are *not* inline-dispatched;
> they are routed **async** via `rh_dispatch_op_async`: the POST returns a
> queued run handle and a detached worker drives the loopback RPC to completion;
> poll status/result at `GET /v1/runs/{id}`. So they keep the listener free
> while still being dedicated `/v1` routes.

`server.health`/`server.info` and `model.list` overlap the existing
`/v1/health`, `/v1/version`, `/v1/models` read-views; the owning wave maps them
to the existing route (no duplicate) and notes it.

## Family → wave map

Counts are methods needing a *new* dedicated route (after exclusions). Each
wave is one delegated packet (`aimee delegate code --persona engineer --verify
"cd src && make server-api-conformance-check && make unit-tests"`), with the
registry + handler files + `--files` preloaded. **Waves are serialized** (each
rebased on `origin/feat/v1-op-parity`) because they all edit
`server_http_routes.inc` + `openapi-server-v1.yaml`; parallel apply-backs to the
same files collide.

| Wave | Families | ~Methods |
|---|---|---|
| **1 (pattern)** | `cron.*` | 8 |
| **2** | `delegate.*`, `job.*`, `jobs.*`, `agent.*`, `episode.list` | ~25 |
| **3** | `provider.*`, `model.*`, `api.*` | ~15 |
| **4** | `dashboard.*`, `insights.overview`, `identity.*`, `dogfood.*`, `lsp.diagnostics_summary` | ~17 |
| **5** | `kb.*`, `curator.*`, `graph.*`, `memory.benchmark`, `index.scan`, `trajectory.batch` | ~12 |
| **6** | `work.*`, `wm.*`, `skill.*`, `session.*`, `toolset.resolve`, `rules.generate`, `blast_radius.preview`, `mcp.*`, `workspace.context`, `worktree.gc`, `aux.test`, `eval.*`, `trigger.*`, `init.run`, `launch.run` | ~40 |

(Method shapes, object vs positional-args vs streaming, are confirmed by the
owning delegate against the preloaded handler; the default is the trivial
`rh_dispatch_op` row.)

## WP1.fin: coverage gate

After the waves, add a conformance gate that walks `server_dispatch_table[]` and
asserts every method has a `/v1` route **or** is on the exclusion list above,
so parity can never silently regress. Wire into `make lint`.
