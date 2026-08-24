#!/usr/bin/env python3
"""Paths cited in prose must exist.

A comment or a document that names a file is a pointer, and nothing follows it
but a human. So when the file moves, the citation stays: the compiler does not
read it, no test asserts it, and it goes on directing readers to somewhere that
is not there. The reader concludes the thing is gone, or worse, that they are
looking in the wrong place.

Found because two sessions hit it the same afternoon. One had an acceptance list
citing `server-go/internal/peer`, a directory that stopped existing when the
code moved into its module. The other -- this tree -- renamed
`server-go/modules/db1` to `server-go/modules/aimee` and deleted
`src/modules/db1`, fixed every import and every Makefile path because those fail
loudly, and left 37 citations in comments and documents that fail silently.

WHAT IT CHECKS: a path-shaped token with a known top-level prefix and a file
extension, or a directory path ending in `/`. It must resolve in the tree.

WHAT IT DOES NOT: judge prose. A line that says a file WAS somewhere is history
and is exempt by the marker below, because a record of a move is exactly the
thing that should keep naming the old location.
"""

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# NARROWED TO MODULE TREES, and the width was the first thing this got wrong.
#
# Scanning every path-shaped token under src/, tests/, docs/ and the rest
# reported 3582 of 9096 citations broken -- 39%, which is not a finding, it is
# a broken instrument. Most were synthetic paths inside test FIXTURES:
# tests/test_guardrails_replay.py names src/config.c and src/ui/status.c as
# sample inputs, and neither is a claim that this repository contains them.
#
# A checker cannot tell a citation from a fixture by looking at the string, so
# it looks at fewer strings: paths under the two MODULE trees, which is where a
# stale one actually misdirects, and which no fixture has reason to invent.
ROOTS = ("src/modules/", "server-go/modules/")
SCAN_SUFFIXES = {".go", ".c", ".h", ".py", ".sh", ".md", ".mk", ".sql"}
# Only comment lines. A path inside a string literal is usually data -- a fixture,
# an argument, a message -- and only a comment is unambiguously a pointer for a
# reader to follow.
COMMENT = re.compile(r"^\s*(//|#|\*|/\*|--|>)")
SCAN_NAMES = {"Makefile", "CONTRIBUTING.md", "README.md"}
SKIP_DIRS = {".git", "build", "obj", "node_modules", ".ci-logs", "vendor", ".venv"}

# A path with an extension, or a directory reference ending in a slash.
CITATION = re.compile(
    r"\b((?:" + "|".join(re.escape(r) for r in ROOTS) + r")[A-Za-z0-9_./-]*"
    r"(?:\.[A-Za-z0-9]+|/))"
)

# A citation on a line that says the thing MOVED or is gone is a record, not a
# pointer. "used to live at", "was", "deleted", "went with", "replaced".
HISTORY = re.compile(
    r"\b(used to|was |were |formerly|previously|deleted|removed|renamed|moved|"
    r"went with|replaced|superseded|no longer|stopped existing|before it moved|"
    r"ported from|ported|port of|lives in git|captured|compiled|compiling|"
    r"HISTORICAL|HISTORY|in git history)\b",
    re.I,
)


def scan_files() -> list[Path]:
    out = []
    for root, dirs, files in os.walk(REPO_ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            path = Path(root) / name
            if path.suffix in SCAN_SUFFIXES or name in SCAN_NAMES:
                out.append(path)
    return out


def main() -> int:
    files = scan_files()
    if not files:
        print("check-cited-paths-exist: read no files; the layout moved")
        return 2

    broken: list[tuple[str, int, str]] = []
    citations = 0
    for path in files:
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, start=1):
            if not COMMENT.search(line):
                continue
            # Prose wraps, so the marker that makes a citation historical is
            # often on an earlier line of the same comment block: "Was a single
            # / src/modules/db1/schema.sql". Looking at one line in isolation
            # reported that as a live pointer to a deleted file.
            block = [line]
            back = lineno - 2
            while back >= 0 and COMMENT.search(lines[back]):
                block.append(lines[back])
                back -= 1
            if any(HISTORY.search(l) for l in block):
                continue
            for cited in CITATION.findall(line):
                cited = cited.rstrip(".,;:)")
                citations += 1
                if (REPO_ROOT / cited).exists():
                    continue
                # A glob or a placeholder is not a claim about one file.
                if any(ch in cited for ch in "*?<>{}"):
                    continue
                # A path in QUOTES inside a comment is a literal being discussed
                # -- a fixture, an expected value, the argument a test passes --
                # rather than somewhere the reader is being sent. closet_test.go
                # quotes "src/modules/git/retry.c" while explaining what a
                # substring check matches; the file's existence is beside the
                # point and its absence is not a broken pointer.
                if f'"{cited}"' in line or f"'{cited}'" in line:
                    continue
                broken.append((str(path.relative_to(REPO_ROOT)), lineno, cited))

    if broken:
        for where, lineno, cited in sorted(broken):
            print(f"    {where}:{lineno}: {cited}")
        print(f"\n{len(broken)} cited path(s) do not exist, of {citations} checked.")
        print("  Nothing follows a prose path but a reader, so a stale one fails")
        print("  silently and sends them somewhere that is not there. Fix the")
        print("  citation, or say the file moved and the line becomes history.")
        return 1

    print(f"check-cited-paths-exist: ok ({citations} citations resolve)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
