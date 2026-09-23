# Link mutation invalidation — 2026-09-23

Schema 34 advances a memory's dependency revision when its outgoing link set
changes. Statement transition tables deduplicate affected parents; source moves
invalidate both old and new sources. Identical updates and timestamp-only edits
produce no canonical event. The existing canonical journal feeds the durable
relation consumer, which queues regeneration. Current source/link checks continue
to withhold invalid copies while that work is pending.

The trigger retains invoker RLS. It cannot rewrite a hidden parent through a
visible child; target deletion already produces its own canonical event and the
consumer follows retained reverse lineage across scopes. Direct hidden-row child
writes are not certified as a complete cross-scope mutation contract by this change.

A replacement inserts its supersession link after its canonical row. That link
now advances the result revision in the same transaction. The store receipt guard
therefore requires both the canonical insertion and the final revision's audit
within the admitted commit. It still binds owner, actor, scope and current result
revision. A regression checks the final revision and exact retry result. Reviewed
model authorship is checked on the canonical insertion rather than requiring that
insertion to be the only memory audit entry in the commit.

[Initial regression evidence](memory-link-journal-2026-09-23/initial-audit-regression.txt)
records the audit assumptions exposed by the new child revision. After repair,
the [full PostgreSQL race suite](memory-link-journal-2026-09-23/go-race.txt) passes
in 131.471 seconds and the [exported owner](memory-link-journal-2026-09-23/export.txt)
passes. Restricted-role link replay covers batched creation, no-op/meaningful
updates, source reassignment, rollback, deletion, parent cascade and journal
revision agreement. Schema reapplication and ownership/bus guards pass.

Fresh candidate `d0e3c5752` passes **1,150/1,150 checks** (130 T1, 1,020 T2).
Both harness processes exit zero. Background-worker cases edit and delete a link
without changing its source content, then verify the stored relationship is rebuilt
or removed. [T1 verdicts](memory-link-journal-2026-09-23/fresh/T1/topology.json),
[T2 verdicts](memory-link-journal-2026-09-23/fresh/T2/topology.json) and
[all nine running image identities](memory-link-journal-2026-09-23/fresh/image-identities.json)
are retained. All three application containers have the actual 32,768-byte provider
cap. Application image: `sha256:03f14f8b9a55db3bde5f26893587554d9a5021b21d1fa64337c6b021b588b915`;
schema-34 PostgreSQL image: `sha256:23c6ec9c2ba1c9d14e6fe5bb94f37914571159e80397fd09701a0c6b37fa9db5`.
The embedder is released 0.4.5. These receipts predate the future-valid admission
follow-up. General child-table mutation coverage,
transitive dependencies and release receipts remain open. These functional suite
timings do not establish a performance improvement.
