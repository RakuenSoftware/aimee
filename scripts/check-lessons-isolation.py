#!/usr/bin/env python3
"""check-lessons-isolation: the graph-feedback S3 retrieval-outcome ledger
(`lessons_*` tables) MUST stay isolated from the memory-fact graph. If a
`lessons_*` table is ever joined into memory-fact recall or the decay/prune
sweep, outcomes leak into normal recall and get pruned out from under the
learning loop (proposal §3). This guard fails if any memory-fact recall or
prune/decay source file references a `lessons_` table.

Enforced as part of `make lint`. See docs/proposals/pending/
graph-feedback-self-audit-and-learning.md §3 and the plan's S3a-schema slice.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Memory-fact recall + decay/prune surfaces. A `lessons_` reference in any of
# these means the ledger has leaked into normal recall or the prune schedule.
GUARDED = [
    "src/modules/db2/c/memory_query.c",            # db2_memory_find_facts_like + fact recall
    "src/modules/db2/c/memory_query_bookkeeping.c",
    "src/modules/db2/c/fact_recall.c",
    "src/modules/db2/c/memory_scope_query.c",
    "src/modules/db2/c/kb_maintenance.c",          # decay / prune sweep
    "src/modules/db2/c/memory_lifecycle.c",
    "src/modules/db2/c/fact_lifecycle.c",
    "src/modules/db2/c/demotion.c",
    "src/modules/db2/c/memory_health.c",
    "src/modules/db2/c/memory_promotion.c",
]

# A `lessons_` token that denotes a table reference (SQL identifier), not an
# unrelated word. Word-boundary + the ledger prefix.
LESSONS_RE = re.compile(r"\blessons_[a-z_]+", re.IGNORECASE)


def scan_text(text):
    hits = []
    for i, line in enumerate(text.splitlines(), 1):
        m = LESSONS_RE.search(line)
        if m:
            hits.append((i, m.group(0), line.strip()))
    return hits


def fail(msg):
    print(f"check-lessons-isolation: FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    # Plant-test: the guard must catch a lessons_ reference in a recall context.
    planted = 'rc = db2_query("SELECT * FROM lessons_outcome_ledger JOIN memory_facts ...");'
    if not scan_text(planted):
        fail("plant-test FAILED — guard did not detect a planted lessons_ reference")
    clean = 'rc = db2_query("SELECT * FROM memory_facts WHERE ...");'
    if scan_text(clean):
        fail("plant-test FAILED — guard flagged a clean memory-fact query")
    print("check-lessons-isolation: plant-test ok")

    violations = []
    scanned = 0
    missing = []
    for rel in GUARDED:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            missing.append(rel)  # renamed/removed — warn so the list doesn't silently rot
            continue
        scanned += 1
        with open(path, encoding="utf-8", errors="replace") as f:
            for ln, tok, src in scan_text(f.read()):
                violations.append(f"{rel}:{ln}: references `{tok}` — {src}")

    if violations:
        print(
            "check-lessons-isolation: FAIL: the retrieval-outcome ledger (lessons_*) "
            "must not appear in memory-fact recall or the prune/decay sweep — it would "
            "leak outcomes into normal recall and prune them out from under the "
            "learning loop (proposal §3):",
            file=sys.stderr,
        )
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)

    for rel in missing:
        print(
            f"check-lessons-isolation: WARN: guarded recall/prune file {rel} not found "
            "(renamed?) — update GUARDED so the isolation invariant keeps covering it",
            file=sys.stderr,
        )
    print(f"check-lessons-isolation: ok ({scanned} recall/prune file(s) clean of lessons_ refs)")


if __name__ == "__main__":
    main()
