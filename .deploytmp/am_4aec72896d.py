"""Graded test for am_4aec72896d (aimee 4aec72896d24).

The .c test below is the one added upstream in that commit. It is injected at
grading time and removed afterwards, so it is never visible while the agent works
and the pristine corpus is left byte-identical for the red gate.

Unlike the other tasks in this corpus, the upstream commit also added the make
rule that builds its test binary. That rule is injected the same way and removed
afterwards. It is APPENDED rather than substituted, so an agent that edited
Rules.mk for its own reasons keeps its edits; only the graded target is added.

Grading is a full build: a compile error is NOT a failing test, and is reported
as such rather than scored as a legitimate red.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TEST_FILES = ['src/tests/test_server_conn_accept.c']
RULES = 'src/tests/Rules.mk'
TEST_TARGET = "build/obj/tests/unit-test-server-conn-accept"
RULE_MARKER = "unit-test-server-conn-accept:"
BUILD_TIMEOUT = int(os.environ.get("PT_BUILD_TIMEOUT", "2400"))


def find_workspace():
    """The candidate checkout, located by shape rather than by sys.path index."""
    for entry in sys.path:
        if not entry:
            continue
        root = Path(entry)
        if (root / "src" / "Makefile").is_file() and (root / "CMakeLists.txt").is_file():
            return root
    raise RuntimeError("no aimee checkout found on sys.path")


class T(unittest.TestCase):
    def test_upstream_regression_tests_pass(self):
        ws = find_workspace()
        here = Path(__file__).resolve().parent / "am_4aec72896d_files"
        restore = []
        try:
            for rel in TEST_FILES:
                dest, src = ws / rel, here / rel
                self.assertTrue(src.is_file(), f"missing graded test file: {rel}")
                restore.append((dest, dest.read_bytes() if dest.exists() else None))
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dest)

            # Append the graded target's build rule if the checkout has no rule
            # for it. Appending (not replacing) keeps whatever the agent did to
            # this file; the marker check keeps a re-run from stacking copies.
            rules_dest = ws / RULES
            frag = (here / 'rules_fragment.mk').read_text()
            current = rules_dest.read_text()
            restore.append((rules_dest, rules_dest.read_bytes()))
            if RULE_MARKER not in current:
                rules_dest.write_text(current + "\n" + frag)

            build = subprocess.run(["make", "-C", "src", TEST_TARGET, "-j8"],
                                   cwd=str(ws), capture_output=True, text=True,
                                   timeout=BUILD_TIMEOUT)
            binary = ws / "src" / TEST_TARGET
            if build.returncode != 0 or not binary.is_file():
                out = (build.stdout or "") + (build.stderr or "")
                self.fail(f"build failed - not a graded result:\n{out[-2500:]}")

            # Match what the unit-tests target does deliberately: point HOME and
            # TMPDIR at a throwaway dir so a test that does not isolate itself
            # cannot read or overwrite real aimee state.
            with tempfile.TemporaryDirectory(prefix="aimee-grade-") as sandbox:
                env = dict(os.environ, HOME=sandbox, TMPDIR=sandbox)
                env.pop("AIMEE_HOME", None)
                proc = subprocess.run([str(binary)], cwd=str(ws), env=env,
                                      capture_output=True, text=True, timeout=600)
        finally:
            for dest, original in restore:
                if original is None:
                    dest.unlink(missing_ok=True)
                else:
                    dest.write_bytes(original)

        combined = (proc.stdout or "") + (proc.stderr or "")
        self.assertEqual(proc.returncode, 0, f"upstream test failed:\n{combined[-3000:]}")
