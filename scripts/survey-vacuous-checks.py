#!/usr/bin/env python3
"""Lint checks that would pass having examined nothing.

The shape found five times in one day: a guard that reports success because its
input was empty. A make target with no prerequisite whose recipe ran nothing; a
stage guard that skipped the module with the most stages and still counted
nineteen others; byte-versus-character assertions on a SQL_ASCII database where
the two functions are identical; a glob resolved cwd-relative under a Makefile
that runs from src/, finding no files; and three checks in this directory that
walked a directory's existence rather than its contents.

All of them read as green.

WHAT THIS LOOKS FOR: a script that collects input by globbing or walking, and
whose success path can be reached with that collection empty. It reports
CANDIDATES, because the question "can this input actually be empty" needs the
script read, and a guard may be spelled in a way no pattern matches.

It is deliberately not a gate. A false positive here would push someone to add a
guard where one exists under another name, and a check that cries wolf about
checks is worse than the gap.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = REPO_ROOT / "scripts"

# Collecting input from the filesystem.
COLLECTS = re.compile(r"\b(glob\.glob|\.glob\(|\.rglob\(|os\.walk|iterdir\()")

# A guard that refuses an empty collection. Spellings vary; these are the ones
# in use here.
GUARDED = re.compile(
    r"if\s+not\s+\w+\s*:|"          # if not files:
    r"==\s*0\s*:|"                  # if scanned == 0:
    r"len\(\w+\)\s*==\s*0|"
    r"if\s+not\s+\w+\.is_dir\(\)|"
    r"proved nothing|having compared nothing|having read nothing|"
    r"having walked nothing|pass vacuously"
)


def main() -> int:
    candidates: list[tuple[str, int]] = []
    scanned = 0
    for path in sorted(SCRIPTS.glob("*.py")):
        if path.name.startswith("survey-"):
            continue
        try:
            body = path.read_text(errors="ignore")
        except OSError:
            continue
        if not COLLECTS.search(body):
            continue
        scanned += 1
        if GUARDED.search(body):
            continue
        candidates.append((path.name, len(body.splitlines())))

    for name, lines in candidates:
        print(f"    {name}  ({lines} lines)")
    print(f"\n{len(candidates)} of {scanned} filesystem-scanning check(s) show no "
          f"empty-input guard.")
    print("  Each needs reading: the question is whether its input can actually be")
    print("  empty, and whether the success path is reachable when it is.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
