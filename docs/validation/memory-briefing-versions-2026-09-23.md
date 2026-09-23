# Briefing source revisions — 2026-09-23

Shared session briefing facts now retain their owner and exact memory revision
from the statement that reads their text. Recent activity retains the episode
revision and its direct parent's revision from the same statement. All IDs and
revisions in these contracts are decimal strings. Invalid or unavailable version
metadata refuses the briefing instead of inventing a successful source binding.

An episode edit changes the episode version with the returned summary. A parent
edit changes the parent version even when the summary text is unchanged. The
existing source revalidation owner rejects each old reference and admits the
new one. Scope/current-validity predicates remain before ranking and row limits;
the complete serialized metadata participates in the existing briefing budget.

The [full PostgreSQL race suite](memory-briefing-versions-2026-09-23/full-race.txt)
passed in 143.515 seconds. Actual non-owner replay covers parent and child edits,
release rechecks, hidden/expired/suppressed exclusions, deterministic rendering
and budgets. The first run exposed an older handcrafted public-runtime fixture
missing collection identity and episode revisions; that fixture was updated to
the current schema contract. Its [failed run](memory-briefing-versions-2026-09-23/initial-failed-race.txt)
is retained. The [exported memory build](memory-briefing-versions-2026-09-23/export.txt)
passed. Fresh-image coverage is pending.

Entity-count aggregates remain unversioned. These returned references do not
constitute transitive lineage closure, an automatic briefing release fence, or a
lock spanning provider dispatch. MR-01/MR-04/MR-06 remain open. CT100 continues
running released 0.4.5.
