#!/usr/bin/env python3
"""Resolve a conflict in a GENERATED baseline by regenerating it.

tests/baselines/refactor/index.json is a pure function of the tree: a digest per
file of the public surface. Two branches that touch the same file both rewrite
the same digest line, so they conflict on every merge -- and the conflict is
never interesting, because NEITHER side's bytes are the answer. The answer is
whatever the merged tree produces.

Run this from a conflicted rebase or merge. It regenerates the baseline from the
tree as it now stands and stages it. Nothing is weakened: CI still recomputes
the baseline and fails if it is wrong.

WHY THIS IS A SCRIPT AND NOT A GIT MERGE DRIVER. A merge driver runs DURING the
merge, when git has not necessarily written every merged file to the working
tree yet -- so a whole-tree digest computed there can be silently wrong. Tried
and measured: with a driver the rebase completed clean and the baseline did not
match the tree, which is strictly worse than the conflict it replaced. Running
AFTER the merge, when the tree is final, is the only sound time to do this.

Usage:
    python3 scripts/resolve_generated_conflicts.py        # regenerate + stage
    python3 scripts/resolve_generated_conflicts.py --check # report, change nothing
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Paths this knows how to derive. A path not listed here is left alone: a
# conflict nobody can regenerate is a conflict a human has to read.
GENERATED = {
    "tests/baselines/refactor/index.json": [
        sys.executable, "-I", "-S", "scripts/refactor_baselines.py", "freeze", "--accept-dirty",
    ],
}


def conflicted_paths() -> list[str]:
    out = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=U"],
        cwd=ROOT, capture_output=True, text=True, check=False,
    )
    return [line.strip() for line in out.stdout.splitlines() if line.strip()]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="report what would be regenerated, change nothing")
    args = parser.parse_args(argv)

    conflicts = conflicted_paths()
    if not conflicts:
        print("resolve-generated: no conflicts")
        return 0

    known = [p for p in conflicts if p in GENERATED]
    unknown = [p for p in conflicts if p not in GENERATED]

    if args.check:
        for p in known:
            print(f"resolve-generated: would regenerate {p}")
        for p in unknown:
            print(f"resolve-generated: needs a human: {p}")
        return 0

    for path in known:
        # Take either side first so the file is not left with conflict markers
        # in it while the generator reads the tree around it.
        subprocess.run(["git", "checkout", "--theirs", "--", path], cwd=ROOT, check=False)
        result = subprocess.run(GENERATED[path], cwd=ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stderr or result.stdout)
            sys.stderr.write(f"resolve-generated: could not regenerate {path}\n")
            return 1
        subprocess.run(["git", "add", "--", path], cwd=ROOT, check=True)
        print(f"resolve-generated: regenerated and staged {path}")

    for path in unknown:
        print(f"resolve-generated: left for you: {path}")

    if unknown:
        print("resolve-generated: resolve the above, then `git rebase --continue`")
    else:
        print("resolve-generated: run `git rebase --continue`")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
