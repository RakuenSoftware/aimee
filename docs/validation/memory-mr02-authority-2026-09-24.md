# MR-02 mutation authority repair — 2026-09-24

A packaged non-owner runtime regression reproduced automatic retirement of 14
protected rows: user/unknown-origin facts and episodes, experiences, instructions
and policies. Shared scheduled and compatibility maintenance now apply the same
model-authority admission as canonical replacement. Automatic lifecycle changes
and folding also preserve protected inputs; generic lifecycle transitions cannot
reactivate terminal history. Eligible model facts still age normally. The pending
lifecycle path also binds its integer TTL explicitly for PostgreSQL.

Bulk export/import now preserves `epistemic_kind`. Imported actor/provenance
claims do not grant authority. Invalid explicit kinds fail instead of silently
becoming world facts. The native import test checks forwarding and failures;
the actual HTTP fixture checks protected conflicts and round-trip kinds.

A second regression reproduced a storage bypass: change an episode's epistemic
kind, then edit its content. Shared schema 36 prohibits removing episode,
experience, instruction or policy protection in place. It preserves annotation,
revocation and replacement paths. The full schema applied twice successfully to
the isolated replay database; append-only private migrations are unchanged.

[Before maintenance](memory-mr02-authority-2026-09-24/maintenance-before.txt),
[before kind guard](memory-mr02-authority-2026-09-24/kind-before.txt),
[passing authority regression](memory-mr02-authority-2026-09-24/authority-after.txt),
[native import](memory-mr02-authority-2026-09-24/native-export.txt) and
[schema upgrade/reapply](memory-mr02-authority-2026-09-24/schema-upgrade.txt)
record the component evidence. The final full race suite passes in 234.086 seconds. Export verification found
the new regression file missing from the explicit module source manifest; after
registering it, the exported owner builds and its tests pass in 5.517 seconds.
The workspace export adapter also now includes primary-scope workspace rows.
Final process validation remains pending; the [eight-gate checklist](../proposals/pending/memory-reliability-02-closeout.md)
tracks MR-02 completion. The original 123-clause acceptance inventory is unchanged.
