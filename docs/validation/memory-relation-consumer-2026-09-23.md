# Durable relation invalidation — 2026-09-23

Schema 33 adds private durable progress for the Go shared-memory relation worker.
Canonical mutations already publish collection-ordered invalidations in the same
commit. The consumer now queues affected canonical roots and advances its cursor
in one transaction. It pages large fan-outs, resumes after restart, serializes
competing workers, and restarts a bounded canonical snapshot after retention gaps
or owner replacement. Retained copied-input observations find dependants even
when deletion has cascaded away their links.

The Go scheduler permits sixteen pages of at most 64 roots per tick. PostgreSQL
owns transaction atomicity and private checkpoint enforcement; Go owns scheduling
and derivation. Runtime callers cannot read or write progress, reset the consumer,
or supply a scope or cursor. The apply helper returns no cross-scope data. Schema
reapplication repairs stale grants. The C bus and memory behavior ownership do
not change; the generated schema catalog's reviewed hashes are updated.

The [full PostgreSQL race suite](memory-relation-consumer-2026-09-23/go-race.txt)
passes in 138.467 seconds. [Focused committed-connection replay](memory-relation-consumer-2026-09-23/targeted-race.txt)
checks private ACLs, bounded snapshots and fan-outs, competing connections,
committed resume, uncommitted disconnect, late queue failure and retry, duplicate
application, retention gaps, target erasure and owner replacement. It also exercises
the Go scheduler adapter. The [exported owner](memory-relation-consumer-2026-09-23/export.txt)
builds and passes its regressions. Fresh image/background-worker validation remains
pending; new deployment cases measure actual regeneration after edits and restart.

Checkpoints certify queued invalidation only. Direct source-version read fences
still decide serving eligibility. This is a purpose-specific relation consumer,
not complete MR-02/MR-04 certification. Link-only mutations, late dependency
registration, arbitrary transitive derivations, time-only activation and full
retention/erasure policy remain work for subsequent changes. No matched latency
claim follows from these functional test timings.
