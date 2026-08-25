#!/usr/bin/env python3
"""Every SQL path that closes its own fact-graph changeset must seal it.

A memory mutation is a changeset: opened, applied, closed. The C mutation API
seals its own closes -- fm_commit_finish() in db2/c/fact_mutation.c appends to
the WORM chain on every close, including revert and ingest-rollback. The
SQL-side closes in schema.sql each had to grow the same call, and a close added
later will not have one unless something checks.

The rule: a statement that sets a terminal status on the FUNCTION'S OWN
changeset (WHERE commit_id=cid) must be followed, inside the same function, by
kb_fact_commit_worm_seal(cid, ...). The revert path's other update targets the
changeset being reverted (WHERE commit_id=p_id); that transition is audited as
the subject of the reverting changeset's own seal, so it is not a close of its
own and is deliberately not matched here.

Exits non-zero when zero close sites resolve. A structural check that silently
matches nothing reports success forever after the shape it greps for moves.

  check_changeset_worm_seal.py [schema.sql]
  check_changeset_worm_seal.py --self-test
"""
import os
import re
import sys

# The schema lives at a fixed place relative to this script, so the check runs
# the same from src/ (where the Makefile invokes it) and from the repo root.
DEFAULT_SCHEMA = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "src", "modules", "db2", "c", "schema.sql")

CLOSE = re.compile(r"UPDATE\s+fact_graph_commits\s+SET\s+status\s*=", re.I)
OWN = re.compile(r"commit_id\s*=\s*cid\b", re.I)
SEAL = re.compile(r"kb_fact_commit_worm_seal\s*\(\s*cid\b", re.I)
FUNC = re.compile(r"^CREATE OR REPLACE FUNCTION\s+([A-Za-z0-9_]+)", re.M)
# A seal has to land inside the same close, not somewhere later in the file.
LOOKAHEAD = 8


def enclosing_function(lines, idx):
    for n in range(idx, -1, -1):
        m = FUNC.match(lines[n])
        if m:
            return m.group(1)
    return "<top level>"


def audit(text):
    """Return (sites, unsealed). A site is (function, 1-based line)."""
    lines = text.splitlines()
    sites, unsealed = [], []
    for i, line in enumerate(lines):
        if not CLOSE.search(line):
            continue
        # The statement runs to its terminating semicolon.
        stmt, j = line, i
        while ";" not in stmt and j + 1 < len(lines) and j - i < LOOKAHEAD:
            j += 1
            stmt += "\n" + lines[j]
        if not OWN.search(stmt):
            continue  # closes somebody else's changeset; audited as a subject
        site = (enclosing_function(lines, i), i + 1)
        sites.append(site)
        window = "\n".join(lines[j + 1: j + 1 + LOOKAHEAD])
        if not SEAL.search(window):
            unsealed.append(site)
    return sites, unsealed


def self_test(text):
    """The check must report a close whose seal line was deleted."""
    sites, unsealed = audit(text)
    if not sites:
        return "self-test: no close sites in the schema to test against"
    if unsealed:
        return "self-test: schema is already unsealed at %s" % (unsealed,)
    stripped = "\n".join(l for l in text.splitlines() if not SEAL.search(l))
    _, after = audit(stripped)
    if len(after) != len(sites):
        return ("self-test: deleting every seal call should leave %d unsealed "
                "closes, the check reported %d" % (len(sites), len(after)))
    return None


def main(argv):
    self_testing = "--self-test" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    path = args[0] if args else DEFAULT_SCHEMA
    with open(path, encoding="utf-8") as fh:
        text = fh.read()

    if self_testing:
        failure = self_test(text)
        if failure:
            print(failure, file=sys.stderr)
            return 1
        print("check_changeset_worm_seal self-test: PASSED")
        return 0

    sites, unsealed = audit(text)
    if not sites:
        print("check_changeset_worm_seal: resolved 0 changeset close sites in %s -- "
              "the check is not looking at anything. Fix the pattern, do not "
              "delete the check." % path, file=sys.stderr)
        return 2
    for func, line in unsealed:
        print("%s:%d: %s closes its own changeset without "
              "kb_fact_commit_worm_seal(cid, ...)" % (path, line, func),
              file=sys.stderr)
    if unsealed:
        print("check_changeset_worm_seal: %d of %d close sites unsealed"
              % (len(unsealed), len(sites)), file=sys.stderr)
        return 1
    print("check_changeset_worm_seal: %d close sites, all sealed" % len(sites))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
