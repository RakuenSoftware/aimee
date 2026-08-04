# Retrieval readiness and recovery

Use `GET /v1/health` only as the process liveness probe. It returns success while a
dependency outage is recoverable, so the supervisor does not restart-loop a healthy
server. Use `GET /v1/ready` to admit or drain retrieval-bearing traffic. Readiness is
false until DB1, KB transport, the KB schema/vector collection, the embedder, and the
E5a dependency breaker can serve the advertised retrieval contract. It also stays
false until every required same-container module process is attached to the server's
local event bus: memory, learning, routing, delegates, tools, workspace, git, skills,
and response-composition.

The shipped Compose files use `restart: unless-stopped` and intentionally healthcheck
`/v1/health`. Orchestrators with separate probes should configure liveness at
`/v1/health` and readiness at `/v1/ready`. A 503 from readiness is an operational
signal, not a reason to kill the process.

The healthcheck is `curl -fsS`, so it reads only the HTTP status. A container can be
`healthy` while the KB inside it cannot embed or search. That is deliberate: a width
mismatch or a missing embedder is not fixed by restarting, and failing the healthcheck
would turn a diagnosable fault into a restart loop. **The body carries the verdict the
HTTP status cannot.** Read `status` and `blockers`:

| `status`    | Meaning                                                    |
|-------------|------------------------------------------------------------|
| `ok`        | The KB can serve retrieval.                                 |
| `degraded`  | It answered and cannot work. `blockers` says why, and names the remedy for each. |

`blockers` is empty exactly when `status` is `ok`. `warnings` is advisory (a stale
ingest, unembedded chunks) and leaves `status` at `ok`. `aimee status` surfaces each
blocker on its own `BLOCKED:` line.

## Diagnose

Run these from a host that can reach the relevant listeners (add authentication or TLS
options for the deployment):

```sh
curl -fsS http://127.0.0.1:8740/v1/health
curl -sS http://127.0.0.1:8740/v1/ready
curl -fsS http://127.0.0.1:8741/v1/health
curl -fsS http://127.0.0.1:8741/v1/pipeline/status
```

The readiness `dependencies` object names the failed boundary. Its `diagnostics`
object reports the E5a breaker state, bounded retry delay, last successful query time
(Unix milliseconds), KB's last successful ingest timestamp, and the first missing
required module process. Pipeline status reports `queue_depth` and
pending/running/failed counts. Preserve all four responses with UTC time, image
digest, and server version before recovery.

## Recover safely

1. If `db1=fail`, verify the server volume is mounted and writable. Restore the mount
   or credentials, then restart only `aimee-server`.
2. If `kb=fail`, verify `aimee-kb` liveness and network/DNS reachability. Recover KB;
   do not repeatedly restart the server. The breaker permits one half-open probe after
   `retry_after_ms`.
3. If `retrieval=fail` with `kb=ok`, inspect KB health fields (`db2_ok`,
   `db2_kb_tables_ok`, `pgvec_ok`, `pgvec_collection_ok`, `embed_ok`). Repair the named
   store/embedder dependency. Never clear or recreate vector data merely to make the
   probe green.
4. If queue depth is growing, stop new ingest, retain failed-job evidence, repair the
   worker dependency, and let the normal queue drain. Use the documented pipeline
   drain endpoint only after preserving status and confirming the operation's limit.
5. If `modules=fail`, inspect `diagnostics.missing_module`, then restore that module
   process and its installed grant in the server container. Do not route module calls
   over the server↔KB network: each daemon admits modules only to its own local bus.
6. Wait at least one readiness sampling interval (default 15 seconds), then require
   `/v1/ready` HTTP 200 and a successful scoped retrieval before restoring traffic.

Do not delete queues, DB volumes, benchmark artifacts, or breaker evidence as a
recovery shortcut. Escalate if the breaker repeatedly reopens or the last successful
ingest/query timestamps do not advance after the dependency is healthy.
