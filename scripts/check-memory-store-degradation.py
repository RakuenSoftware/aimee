#!/usr/bin/env python3
"""No memory path may depend on the relational store and say NOTHING when it is gone.

The pattern found in memory_graph_fusion.c: a function calls db2_* helpers that
each guard internally and return empty, so the caller yields no results and
cannot distinguish that from "there genuinely are none". On aimee-server, which
links no libpq, that is the normal case rather than an edge case.

Classifies each memory translation unit that touches db2:

  FORKED   has #if AIMEE_DB2_DISABLED -- the no-store case is a deliberate branch
  LOGGED   has a LOG_ call -- at least capable of saying something
  SILENT   neither -- a defect, and what this gate refuses

Found once, in memory_graph_fusion.c: expansion returned 0 results on
aimee-server (which links no libpq) with no indication why, a result
indistinguishable from "this memory genuinely has no neighbours". Every other
db2-touching memory file already branched on AIMEE_DB2_DISABLED.

This gate has a limited life: after the S5 cutover puts PostgreSQL on both
daemons there is no unreachable-store case on the server, and the fork it counts
disappears with it. Until then a new memory file that reaches the store without
either branching or reporting is a regression, and this is what catches it.
"""
import sys
import re
from pathlib import Path

from source_text import strip_comments_text

ROOT = Path(__file__).resolve().parents[1]

# SCOPED TO MEMORY, AND THE RULE IS ONLY TRUE HERE. Widening this path is a
# one-word edit and it is the wrong instinct, because the reasoning above does
# not generalise: an empty memory recall is indistinguishable from a genuine
# absence, which is why silence is the harm. Most code that reaches db2 SHOULD
# fail hard rather than degrade, and for it a fork would be the defect.
#
# Measured rather than asserted: 442 files outside this directory call db2_*,
# and under this rule 406 of them would be reported SILENT. A guard that reports
# 406 defects is not a strict guard, it is a broken one -- indistinguishable, to
# anyone reading its output, from a guard that has stopped working, and it will
# be switched off rather than answered.
#
# The distinction is invisible from inside this file, because within
# src/modules/memory a db2 call with no fork genuinely IS a defect every time.
# That coincidence is what makes widening look safe.
MEM = ROOT / "src/modules/memory"

DB2_CALL = re.compile(r"\b(db2_[a-z0-9_]+)\s*\(")
# The shared-memory migration preserves a number of db2_* ABI entry points in
# transport adapters.  Those functions call the memory module; their names do
# not mean the file reaches DB2.  Ignore functions implemented by the same
# translation unit, while continuing to flag calls to external db2_* owners.
DB2_DEFINITION = re.compile(
    r"^\s*(?:[A-Za-z_][A-Za-z0-9_]*\s+)+(db2_[a-z0-9_]+)\s*\(", re.MULTILINE
)
DB2_ADAPTER_FILES = {
    "memory_data_bus.c",
    "memory_domain_bus.c",
    "memory_domain_runtime_bus.c",
    "memory_scope_connection.c",
}
adapter_definitions = set()
for adapter_name in DB2_ADAPTER_FILES:
    adapter = MEM / adapter_name
    if adapter.exists():
        adapter_definitions.update(
            DB2_DEFINITION.findall(strip_comments_text(adapter.read_text(encoding="utf-8")))
        )
rows = []
for path in sorted(MEM.glob("*.c")):
    # Comments stripped FIRST, and this is the whole point of the check rather
    # than tidiness. Both tests below are satisfied by a file merely TALKING
    # about degrading: a TODO promising to branch on AIMEE_DB2_DISABLED and to
    # LOG_WARN once made a file that did neither come back clean. A guard that
    # reads intent as implementation is worse than no guard, because it reports
    # the gap as covered.
    text = strip_comments_text(path.read_text(encoding="utf-8", errors="replace"))
    local_definitions = set(DB2_DEFINITION.findall(text))
    calls = sum(
        1
        for name in DB2_CALL.findall(text)
        if name not in local_definitions and name not in adapter_definitions
    )
    if not calls:
        continue
    forked = "AIMEE_DB2_DISABLED" in text
    logged = bool(re.search(r"\bLOG_(WARN|ERROR|INFO)\s*\(", text))
    if forked:
        state = "FORKED"
    elif logged:
        state = "LOGGED"
    else:
        state = "SILENT"
    rows.append((state, path.name, calls))

for state in ("SILENT", "LOGGED", "FORKED"):
    group = [r for r in rows if r[0] == state]
    print(f"{state}: {len(group)} file(s)")
    for _, name, calls in sorted(group, key=lambda r: -r[2]):
        print(f"    {name:<40} {calls:>4} db2 call(s)")
silent = [r for r in rows if r[0] == "SILENT"]
if silent:
    print(
        "\ncheck-memory-store-degradation: FAIL -- these files reach the relational "
        "store but neither branch on AIMEE_DB2_DISABLED nor report when it is "
        "unavailable, so they return empty results indistinguishable from a genuine "
        "absence:",
        file=sys.stderr,
    )
    for _, name, calls in silent:
        print(f"  src/modules/memory/{name} ({calls} db2 calls)", file=sys.stderr)
    print(
        "  Fix: branch on the fork like the other 22 files, or probe the store once "
        "and warn once (see memory_graph_fusion.c and src/db1_client/git_ownership.c).",
        file=sys.stderr,
    )
    raise SystemExit(1)

print(
    f"check-memory-store-degradation: ok ({len(rows)} memory files touch db2; "
    f"0 silent)"
)
