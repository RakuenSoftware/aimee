"""Count real code change in a patch, not raw diff '+' lines.

WHY THIS EXISTS. The obvious metric -- count lines starting with '+' -- inflates
in three separate ways, and all three fire on real patches:

  comments/blanks  a five-line comment above a two-line fix reads as 7 lines
  includes/imports bookkeeping, not logic
  MOVED code       a block relocated within a file counts as both + and -, so a
                   pure move reads as new work while the net is zero

Measured on am_312e901904: the raw count says aimee wrote 38 production lines.
Of those, src/posix/util.c contributes 7 -- but its 2 statements are the SAME
two statements deleted five lines below, relocated. The real production change
there is zero net lines; the fix is the ordering.

Reports both so the gap is visible rather than hidden behind one number.
"""
import os
import re
import sys
from collections import defaultdict

COMMENT = re.compile(r"^\s*(/\*|\*|//|#(?!include)|--)|^\s*$")
INCLUDE = re.compile(r"^\s*(#include|import |from \s|use )")


def is_test(path):
    base = os.path.basename(path)
    return ("/tests/" in path or path.startswith("tests/")
            or base.startswith("test_") or base.endswith("_test.go"))


def classify(line):
    body = line[1:]
    if COMMENT.match(body):
        return "comment"
    if INCLUDE.match(body):
        return "include"
    return "code"


def analyse(diff_path):
    cur = None
    added = defaultdict(list)
    removed = defaultdict(list)
    for line in open(diff_path, errors="replace"):
        m = re.match(r"^\+\+\+ b/(.+)$", line)
        if m:
            cur = m.group(1).strip()
            continue
        if line.startswith(("--- ", "+++ ", "@@", "diff ", "index ")) or cur is None:
            continue
        if line.startswith("+"):
            added[cur].append(line.rstrip("\n"))
        elif line.startswith("-"):
            removed[cur].append(line.rstrip("\n"))
    return added, removed


def norm(line):
    return re.sub(r"\s+", " ", line[1:]).strip()


def report(diff_path, label):
    added, removed = analyse(diff_path)
    tot = defaultdict(int)
    print("\n=== %s ===" % label)
    print("%-46s %5s %5s %5s %5s %6s" % ("file", "raw+", "code", "cmnt", "incl", "moved"))
    for path in sorted(set(added) | set(removed)):
        a, r = added[path], removed[path]
        rset = {norm(x) for x in r}
        code = cmnt = incl = moved = 0
        for line in a:
            kind = classify(line)
            if kind == "comment":
                cmnt += 1
            elif kind == "include":
                incl += 1
            elif norm(line) in rset:
                moved += 1          # same statement deleted elsewhere in this file
            else:
                code += 1
        bucket = "test" if is_test(path) else "prod"
        tot[bucket + "_raw"] += len(a)
        tot[bucket + "_code"] += code
        tot[bucket + "_cmnt"] += cmnt
        tot[bucket + "_incl"] += incl
        tot[bucket + "_moved"] += moved
        print("%-46s %5d %5d %5d %5d %6d" % (path[-46:], len(a), code, cmnt, incl, moved))
    print("-" * 76)
    for b in ("prod", "test"):
        print("%-46s %5d %5d %5d %5d %6d"
              % (b.upper() + " total", tot[b + "_raw"], tot[b + "_code"],
                 tot[b + "_cmnt"], tot[b + "_incl"], tot[b + "_moved"]))


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        report(arg, arg)
