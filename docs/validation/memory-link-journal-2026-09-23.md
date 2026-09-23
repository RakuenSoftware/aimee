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

Fresh deployment validation remains pending. New background-worker checks edit
and delete a link without changing its source content, then wait for the stored
relationship to be rebuilt or removed. General child-table mutation coverage,
transitive dependencies and release receipts remain open. These functional suite
timings do not establish a performance improvement.
