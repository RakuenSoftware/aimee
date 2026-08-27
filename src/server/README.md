# aimee-server

This directory owns the C resource plane: DB1-facing sessions, agents, tools, policy, vault,
provider calls, KB clients, and the public server `/v1` surface. Physical DB1 storage belongs to
the `aimee` and `postgres` modules.

It does not own DB2 or workflow lifecycle.

## Boundaries

- reaches DB1 only through the module bus and never links libpq;
- retains SQLite only for the separate append-only audit WORM;
- reaches DB2 only through the typed KB client;
- returns `410 Gone` for retired C workflow lifecycle routes;
- serves local clients over a filesystem-protected Unix socket;
- serves remote clients through TLS, identity, capabilities, scope, and write-tier checks;
- exposes narrow internal resource operations to the supervised Go workflow peer.

## Main areas

| Area | Responsibility |
| --- | --- |
| HTTP/listeners | parse, authenticate, route, stream, request IDs, shutdown |
| DB1/state | sessions, working memory, jobs, policy/audit state, caches |
| compute | bounded blocking work and provider calls |
| agents | admission, role routing, canonical IR, tool loop, retries, accounting |
| policy/tools | schemas, guardrails, worktree/path checks, backend execution |
| vault | credential resolution, custody, rotation, access audit |
| KB client | typed server-to-KB operations; no SQL |
| observability bus | action and guardrail publication, durable sink, capture |
| forge resource | confined mechanical operations requested by `aimee-wfe` |

## Concurrency

Listeners hand blocking work to bounded pools. Heavy provider work also passes an admission budget.
Event-bus publishers are serialized per SPSC producer; a dedicated consumer pumps, drains, writes
typed sinks, and flushes capture. Shutdown rejects new work, waits for in-flight publishers, drains,
then destroys the bus and database state.

Do not add an unbounded queue or a second workflow writer to solve local pressure.

## Checks

```bash
make -C src check-linking
make -C src module-boundary-check
make -C src server-api-conformance-check
make -C src unit-tests
```

New remote routes need negative authentication, capability, scope, and write-tier tests. New internal
forge operations also need peer identity, repository, worktree, branch, and argument confinement.

See [Architecture](../../docs/ARCHITECTURE.md), [Security](../../docs/SECURITY.md), and
[Technical reference](../README.md).
