# MR-02 mutation authority repair — 2026-09-24

Final status: [MR-02 is complete](memory-mr02-closeout-2026-09-24.md). The entries
below preserve intermediate reproductions and validation checkpoints.

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
Final process validation passed; the [eight-gate checklist](../proposals/pending/memory-reliability-02-closeout.md)
tracks MR-02 completion. The original 123-clause acceptance inventory is unchanged.


The actual HTTP fixture exposed the legacy C export path returning
`kb.export: export failed`. Filtered export now runs in the Go owner's scoped
transaction; C only forwards request and response. Memory payloads retain
`epistemic_kind`, primary workspace and exact IDs. Entity metadata is rebuilt
from the exported parents, and unversioned global profile-card payloads are
omitted (`card_json` stays `{}`); they cannot establish a scoped source. The
packaged non-owner fixture verifies protected kinds and foreign-workspace
exclusion through both the store and host runtime. Native transport refuses an
unavailable owner without database fallback. The final image/process replay passed; see the closeout above.


The native adapters now pass the KB verifier's captured request context explicitly
for both import and export. Body authority fields stay separate from that trusted
context. The native regression checks scope forwarding; the scoped-credential
HTTP fixture covers inherited project/workspace audience and foreign import
refusal. The final Go owner race suite passes in 235.243 seconds and exported
owner verification in 6.214 seconds. Fresh image validation passed; see the closeout above.

## Final process-fixture corrections

The scoped fixture initially inserted its new import record before existing
fixed-count dashboard assertions. `aa87ca198` moves only the mutating import
checks after those assertions and pooled reads; the original counts remain
unchanged. The corrected project/workspace fixture passed all 36 checks.

The first final-image T2 run reached the native refresh-owner outage but exceeded
the fixture's 90-second terminal-state polling window. Its
[partial native evidence](memory-mr02-authority-2026-09-24/t2-before-outage-window-native-async.json)
and [failed topology result](memory-mr02-authority-2026-09-24/t2-before-outage-window-topology.json)
are retained. T3 completed that same outage case in 78.56 seconds and passed all
617 checks. `863ae9f14` gives only the deliberately paused-owner refresh case a
bounded 180-second observation window and records explicit timeout evidence.
Refusal, no-sixth-send, recovery and durable receipt assertions are unchanged.
