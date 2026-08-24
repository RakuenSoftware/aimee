#!/usr/bin/env python3
"""Scripts that nothing in the tree invokes or mentions.

The fourth shape of the same defect found three times today: a test target in no
run list, a test source no target builds, a check target nothing reaches, and
now a script nothing calls. Each exists, is reviewed, is cited in commit
messages, and never runs.

WHAT COUNTS AS A REFERENCE: the script's path or basename appearing anywhere in
the tree other than the script itself -- a Makefile recipe, a workflow step, a
Dockerfile, another script, or documentation. Documentation counts because a
script a document tells a human to run is a tool, not a corpse.

The first version of this got it wrong by counting occurrences INCLUDING the
script's own file and treating "at most one" as unreferenced. Most scripts do
not name themselves, so anything referenced exactly once -- the common case for
a lint gate wired into one Makefile line -- was reported as dead. It listed 66
of 235, including scripts visibly invoked by ci.yml. Excluding the file itself
and requiring zero references is the fix, and the difference is a survey nobody
could have acted on versus one they can.

Reports, does not enforce. A script may be deliberately kept for an operator to
run by hand, and telling those apart from abandoned ones needs the reason.
"""

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = REPO_ROOT / "scripts"

# Where a reference could live. Everything textual, minus the noise.
SEARCH_SUFFIXES = {".py", ".sh", ".mk", ".yml", ".yaml", ".md", ".json", ".c", ".h", ".go"}
SEARCH_NAMES = {"Makefile", "Dockerfile"}
SKIP_DIRS = {".git", "build", "obj", "node_modules", ".ci-logs", "vendor"}


def corpus() -> dict[Path, str]:
    """Every readable text file, by path, so a file can be excluded by identity."""
    out: dict[Path, str] = {}
    for root, dirs, files in os.walk(REPO_ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".venv")]
        for name in files:
            path = Path(root) / name
            if path.suffix in SEARCH_SUFFIXES or name in SEARCH_NAMES \
               or name.startswith("Dockerfile"):
                try:
                    out[path] = path.read_text(errors="ignore")
                except OSError:
                    continue
    return out


def main() -> int:
    files = corpus()
    if not files:
        print("survey-unreferenced-scripts: read no files; the layout moved")
        return 2

    scripts = sorted(p for p in SCRIPTS.rglob("*") if p.suffix in {".py", ".sh"})
    if not scripts:
        print(f"survey-unreferenced-scripts: no scripts under {SCRIPTS}")
        return 2

    unreferenced = []
    for script in scripts:
        name = script.name
        found = False
        for path, body in files.items():
            if path == script:
                continue  # its own file is not a reference to itself
            if name in body:
                found = True
                break
        if not found:
            unreferenced.append(script.relative_to(REPO_ROOT))

    for path in unreferenced:
        print(f"    {path}")
    print(f"\n{len(unreferenced)} of {len(scripts)} script(s) are named nowhere else in the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
