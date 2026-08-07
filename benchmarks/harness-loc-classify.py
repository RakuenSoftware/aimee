#!/usr/bin/env python3
"""Classify test files by path SHAPE, not by a literal `tests/` prefix.

Applies to ponytail-codex-benchmark/battery/codex_matrix_runner.py.

WHY. Two sites decided test-vs-production with:

    rel.startswith("tests/")

which is true only for a top-level `tests/` directory. On the am_ corpus that is
true for nothing. Measured on am_312e901904, where the agent wrote 44 lines of
tests across two files:

    src/tests/test_git_cred_inject.c      -> classified production
    frontend/src/setup/ownerUrl.test.ts   -> classified production
    reported: test_added = 0, test_files = []

So the cell that produced the strongest result in the run -- the only arm to
write any tests at all -- reported writing none, and every test line it wrote
was added to its production line count instead. Both halves of that are wrong
and they compound: production is inflated by exactly the amount test is
understated.

This is the same blind spot the test gate had (it knew `_test.ts` but not
`.test.ts`), so the pattern is shared rather than written twice.

Idempotent: re-running detects the already-patched form and does nothing.
"""
import ast
import py_compile
import re
import sys
from pathlib import Path

RUNNER = Path("/opt/bench/ponytail-codex-benchmark/battery/codex_matrix_runner.py")

# Same shape the gate uses. Kept as one definition injected once, so the two
# classifier sites and the gate cannot drift apart again.
HELPER = '''

# --- test path classification ----------------------------------------------
# `tests/` alone matched nothing on a real repository layout. Covers a tests/ or
# test/ directory at ANY depth (src/tests/...), test_foo.py, foo_test.go, and
# the JS/TS foo.test.ts / foo.spec.tsx convention.
LOC_TEST_RE = re.compile(
    r"(^|/)tests?/"
    r"|(^|/)test_[^/]+$"
    r"|_test\\.(go|py|ts|tsx|js|jsx)$"
    r"|\\.(test|spec)\\.[jt]sx?$"
)


def is_test_path(rel):
    return bool(LOC_TEST_RE.search(rel))
# --- end test path classification -------------------------------------------
'''

SITES = [
    ('    production = [path for path in changed if not path.startswith("tests/")]\n'
     '    tests = [path for path in changed if path.startswith("tests/")]',
     '    production = [path for path in changed if not is_test_path(path)]\n'
     '    tests = [path for path in changed if is_test_path(path)]'),
    ('        bucket = "test" if rel.startswith("tests/") else "production"',
     '        bucket = "test" if is_test_path(rel) else "production"'),
]


def main():
    text = RUNNER.read_text()
    if "def is_test_path(" in text:
        print("already patched")
        return 0

    missing = [old for old, _ in SITES if old not in text]
    if missing:
        print("FATAL: classifier site(s) not found; refusing to patch blind")
        for old in missing:
            print("  missing:", old.splitlines()[0].strip())
        return 2

    RUNNER.with_suffix(".py.pre-locclass.bak").write_text(text)

    anchor = "def diff_quality("
    text = text.replace(anchor, HELPER.strip() + "\n\n\n" + anchor, 1)
    for old, new in SITES:
        text = text.replace(old, new, 1)

    RUNNER.write_text(text)

    problems = []
    try:
        py_compile.compile(str(RUNNER), doraise=True)
    except py_compile.PyCompileError as exc:
        problems.append("does not compile: %s" % exc)

    body = RUNNER.read_text()
    if 'startswith("tests/")' in body:
        problems.append("a startswith(\"tests/\") classifier survived the patch")
    # The helper must be defined before its first use, or it is a NameError at
    # grading time -- the failure mode that cost three cells already.
    defined = body.find("def is_test_path(")
    first_use = min((body.find(new.strip().splitlines()[0]) for _, new in SITES
                     if body.find(new.strip().splitlines()[0]) >= 0), default=-1)
    if defined < 0 or (first_use >= 0 and first_use < defined):
        problems.append("is_test_path used before it is defined")

    if problems:
        RUNNER.write_text(RUNNER.with_suffix(".py.pre-locclass.bak").read_text())
        for p in problems:
            print("FATAL:", p)
        print("reverted:", RUNNER)
        return 3

    print("patched:", RUNNER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
