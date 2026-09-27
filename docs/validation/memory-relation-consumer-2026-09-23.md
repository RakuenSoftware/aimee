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
builds and passes its regressions. Fresh candidate `1baa020a6` passes **1,146/1,146 checks** (128 T1, 1,018 T2),
including actual background generation, copied-input mutation rebuilding and
application restart in both KB topologies. Both harness processes exit zero.
[Individual T1 verdicts](memory-relation-consumer-2026-09-23/fresh/T1/topology.json),
[T2 verdicts](memory-relation-consumer-2026-09-23/fresh/T2/topology.json) and
[all nine running image identities](memory-relation-consumer-2026-09-23/fresh/image-identities.json)
are retained with the remaining child-suite receipts. All three application
containers have the actual 32,768-byte provider cap. Application image:
`sha256:ecf418571808af169fd83b7344d13f53e6ca93e12281802fdda1146ab962973a`;
schema-33 PostgreSQL image:
`sha256:895857f22594f2ca0653f3495f7382e60867e88274ca02c4239f3d8784c15e70`.
The embedder is released 0.4.5. These receipts predate schema-34 link revisions
and the future-valid indexing follow-up.

Checkpoints certify queued invalidation only. Direct source-version read fences
still decide serving eligibility. This is a purpose-specific relation consumer,
not complete MR-02/MR-04 certification. Link-only mutations, late dependency
registration, arbitrary transitive derivations, time-only activation and full
retention/erasure policy remain work for subsequent changes. No matched latency
claim follows from these functional test timings.
