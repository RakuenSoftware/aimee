#!/usr/bin/env python3
"""Run the scripts/tests/ tests that no workflow or Makefile names.

The workflows invoke these individually -- `python3 -I scripts/tests/test_x.py`
-- so the set that runs is an enumerated list, and a test added without editing
a workflow runs nowhere. This finds those and runs them, which is the only way
to tell idle coverage from stale code.
"""

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def named() -> set[str]:
    found: set[str] = set()
    sources = glob.glob(str(REPO_ROOT / ".github/workflows/*.yml"))
    sources += [str(REPO_ROOT / "src" / "Makefile")]
    sources += glob.glob(str(REPO_ROOT / "src" / "**" / "*.mk"), recursive=True)
    for path in sources:
        try:
            found.update(re.findall(r"(test_[a-z0-9_]+\.py)", Path(path).read_text(errors="ignore")))
        except OSError:
            continue
    return found


def main() -> int:
    tests = sorted(Path(p) for p in glob.glob(str(REPO_ROOT / "scripts/tests/test_*.py")))
    unnamed = [t for t in tests if t.name not in named()]
    if not unnamed:
        print("run-unnamed-script-tests: every scripts/tests test is named somewhere")
        return 0

    failures = []
    for test in unnamed:
        proc = subprocess.run([sys.executable, "-I", str(test)],
                              capture_output=True, text=True, cwd=REPO_ROOT, timeout=300)
        verdict = "PASS" if proc.returncode == 0 else f"FAIL(rc={proc.returncode})"
        print(f"{test.name:48s} {verdict}")
        if proc.returncode != 0:
            tail = (proc.stderr or proc.stdout).strip().splitlines()[-3:]
            for line in tail:
                print(f"      {line[:120]}")
            failures.append(test.name)

    print(f"\n{len(unnamed)} unnamed test(s), {len(failures)} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
