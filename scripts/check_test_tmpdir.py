#!/usr/bin/env python3
"""Test temp paths must honour TMPDIR. Ratchet: existing sites are debt.

The unit-test runner (src/tests/Rules.mk) exports TMPDIR into every test and
removes that directory on exit, so anything created through it is cleaned up.
A test that hardcodes "/tmp/..." escapes that sandbox and leaks one entry per
run, forever.

This is not theoretical and it is not small. Measured on the development box:

    /tmp entries                    44105
    aimee test leftovers            39619

The suite's own notes record where that ends -- "they used to pile up in /tmp
across every run until tmpfs ran out of INODES (40k dirs, 857k inodes, with 45GB
still free) and nothing could create a file". The count above is already at that
threshold. The leakage is spread across ~130 test files at roughly one entry per
file per run, so it is a slow, even drip rather than a few bad actors: the top 25
prefixes account for only 27% of it.

The baseline below is DEBT, not permission. It may only fall. Fix a test to
build its path from TMPDIR (getenv("TMPDIR") or platform_tmpdir()), run
--update-baseline, commit.

BUT NOT EVERY LITERAL IS A LEAK, and "empty baseline" is the wrong target.
A "/tmp/..." string only leaks if something CREATES it. Some are fixtures --
data fed to code that only reasons about paths -- and converting one changes
what the test asserts:

  pre_tool_check(..., "/tmp/.aimee/worktrees/test/main", msg, sizeof(msg))
      a path being JUDGED by the guardrail under test, created by nothing
  attn_session_isolation_blocked(..., "/tmp/.aimee-xyz/src/x.c", ...)
      likewise -- the string IS the input
  "/tmp/aimee-missing-delegate-worktree"
      must NOT exist; its absence is the whole point of the case

Judge the SITE, never the file. The same file usually holds both: in
test_guardrails.c the fixtures above sat a few hundred lines from
`char tmpdir[] = "/tmp/test_wt_branch_XXXXXX"; mkdtemp(tmpdir);` -- 36 real
leaks in the file, now fixed, with the fixtures left exactly as they were.

So the floor is not zero, but it is much nearer zero than the raw count
suggests. The sharpest signal is an "XXXXXX" template: it exists only to be
handed to mkdtemp/mkstemp, so it is always created, and ~216 of the sites still
recorded here carry one. Before you skip a site, look for
mkdtemp/mkstemp/mkdir/fopen/system on that path; if something creates it, it is
debt, not a fixture.

Note also that ~64 of those template sites live in tests owned by a vendored
module (see tests/baselines/refactor/module-test-registration.json). Fixing one
re-pins that module's mirror, which is a deliberate act -- not hygiene.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "src", "tests")
BASELINE = os.path.join(ROOT, "scripts", "test-tmpdir-baseline.json")

# A string literal that begins with /tmp/. Matches the mkstemp/mkdtemp template
# forms ("/tmp/foo-XXXXXX") and plain paths alike.
LITERAL = re.compile(r'"/tmp/[^"]*"')


def scan() -> dict[str, int]:
    found: dict[str, int] = {}
    for dirpath, _dirs, files in os.walk(TESTS):
        for name in sorted(files):
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8", errors="replace") as fh:
                hits = LITERAL.findall(fh.read())
            if hits:
                found[os.path.relpath(path, ROOT)] = len(hits)
    return found


def load_baseline() -> dict[str, int]:
    if not os.path.exists(BASELINE):
        return {}
    with open(BASELINE, encoding="utf-8") as fh:
        return json.load(fh).get("files", {})


def write_baseline(found: dict[str, int]) -> None:
    with open(BASELINE, "w", encoding="utf-8") as fh:
        json.dump(
            {
                "_comment": (
                    "Ratchet baseline for check_test_tmpdir.py. Each entry is a test "
                    "that hardcodes /tmp instead of honouring TMPDIR, and so leaks one "
                    "entry per suite run. DEBT, not permission: these counts may only "
                    "fall. Fix a test, run --update-baseline, commit."
                ),
                "files": dict(sorted(found.items())),
            },
            fh,
            indent=2,
        )
        fh.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update-baseline", action="store_true")
    args = parser.parse_args()

    found = scan()
    if args.update_baseline:
        write_baseline(found)
        total = sum(found.values())
        print(f"check_test_tmpdir: baseline updated ({len(found)} files, {total} sites)")
        return 0

    baseline = load_baseline()
    regressions = []
    for path, count in sorted(found.items()):
        allowed = baseline.get(path, 0)
        if count > allowed:
            regressions.append(f"{path}: {count} hardcoded /tmp path(s), baseline {allowed}")

    if regressions:
        print("check_test_tmpdir: error: new hardcoded /tmp path(s) in tests.", file=sys.stderr)
        print(
            "A test that hardcodes /tmp escapes the runner's TMPDIR sandbox and leaks one\n"
            "entry per run; /tmp already holds ~40k of them. Build the path from TMPDIR\n"
            'instead: getenv("TMPDIR") (falling back to "/tmp") or platform_tmpdir().\n'
            "If nothing CREATES the path -- it is a fixture string fed to code that only\n"
            "reasons about paths -- leave it as it is and record it with --update-baseline.",
            file=sys.stderr,
        )
        for line in regressions:
            print(f"  {line}", file=sys.stderr)
        return 1

    stale = sorted(set(baseline) - set(found))
    improved = [p for p in sorted(found) if found[p] < baseline.get(p, 0)]
    if stale or improved:
        print(
            "check_test_tmpdir: ok — and the debt fell; run "
            "scripts/check_test_tmpdir.py --update-baseline to record it "
            f"({len(stale)} file(s) clean, {len(improved)} reduced)"
        )
        return 0

    total = sum(found.values())
    print(f"check_test_tmpdir: ok ({len(found)} files, {total} sites, ratchet holding)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
