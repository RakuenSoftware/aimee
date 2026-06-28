#!/usr/bin/env python3
"""Unit tests for scripts/dev-accept.py (autonomous-dev §3/§4/§5).

Self-contained: synthetic acceptance text + a synthetic git tree in a tmpdir, no
network, no live CI. Run:
    python3 -m unittest discover -s scripts/tests -p test_dev_accept.py
"""
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "dev-accept.py"
spec = importlib.util.spec_from_file_location("dev_accept", SCRIPT)
da = importlib.util.module_from_spec(spec)
spec.loader.exec_module(da)


def accept(*entries):
    return "```yaml acceptance\n" + "\n".join(entries) + "\n```\n"


def deferred(*entries):
    return "```yaml deferred\n" + "\n".join(entries) + "\n```\n"


class Evaluate(unittest.TestCase):
    def test_all_mechanical_pass(self):
        text = accept('- {id: 1, tier: mechanical, check: "true"}',
                      '- {id: 2, tier: mechanical, check: "true"}')
        v = da.evaluate(text)
        self.assertEqual(v["verdict"], "passed")
        self.assertEqual(v["reason"], "ok")
        self.assertEqual(v["total"], 2)

    def test_one_mechanical_fail(self):
        text = accept('- {id: 1, tier: mechanical, check: "true"}',
                      '- {id: 2, tier: mechanical, check: "false"}')
        v = da.evaluate(text)
        self.assertEqual(v["verdict"], "failed")
        self.assertEqual(v["reason"], "check-failed")

    def test_deployment_undeferred_blocks(self):
        # deployment/hardware not declared deferred + no --dispatch -> validation-pending
        text = accept('- {id: 1, tier: mechanical, check: "true"}',
                      '- {id: 2, tier: deployment, check: "ci:e2e"}')
        v = da.evaluate(text, dispatch=False)
        self.assertEqual(v["verdict"], "filing-blocked")
        self.assertEqual(v["reason"], "validation-pending")
        dep = [c for c in v["checks"] if c["tier"] == "deployment"][0]
        self.assertEqual(dep["status"], "validation-pending")

    def test_deployment_declared_deferred_does_not_block(self):
        text = (accept('- {id: 1, tier: mechanical, check: "true"}',
                       '- {id: 2, tier: deployment, check: "ci:e2e"}')
                + deferred('- {tier: deployment, reason: "needs live forge", '
                           'deferred_to: full-autonomous-development.md}'))
        v = da.evaluate(text)
        self.assertEqual(v["verdict"], "passed")
        dep = [c for c in v["checks"] if c["tier"] == "deployment"][0]
        self.assertEqual(dep["status"], "skipped-declared")
        self.assertFalse(dep["applicable"])
        self.assertEqual(dep["deferred_to"], "full-autonomous-development.md")

    def test_no_acceptance_block(self):
        v = da.evaluate("no blocks here")
        self.assertEqual(v["verdict"], "failed")
        self.assertEqual(v["reason"], "no-acceptance-block")

    def test_mechanical_timeout_is_unavailable(self):
        text = accept('- {id: 1, tier: mechanical, check: "sleep 5"}')
        v = da.evaluate(text, timeout=1)
        self.assertEqual(v["checks"][0]["status"], "unavailable")
        self.assertEqual(v["verdict"], "filing-blocked")


class FileToDone(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = Path(self.tmp)
        (self.root / "docs/proposals/pending").mkdir(parents=True)
        (self.root / "docs/proposals/done").mkdir(parents=True)
        self.prop = self.root / "docs/proposals/pending/foo.md"
        self.prop.write_text("# Foo\n", encoding="utf-8")
        (self.root / "docs/proposals/pending/foo.plan.md").write_text("plan\n", encoding="utf-8")
        # a doc that references the pending path (link-graph fixup target)
        self.index = self.root / "docs/PROPOSALS.md"
        self.index.write_text("see pending/foo.md for details\n", encoding="utf-8")
        subprocess.run(["git", "-C", self.tmp, "init", "-q"], check=True)
        subprocess.run(["git", "-C", self.tmp, "add", "-A"], check=True)
        subprocess.run(["git", "-C", self.tmp, "-c", "user.email=t@t", "-c",
                        "user.name=t", "commit", "-qm", "init"], check=True)
        self._orig_root = da.REPO_ROOT
        da.REPO_ROOT = self.tmp

    def tearDown(self):
        da.REPO_ROOT = self._orig_root

    def test_refuses_unless_passed(self):
        ok, msg = da.file_to_done(str(self.prop), {"verdict": "filing-blocked"})
        self.assertFalse(ok)
        self.assertTrue(self.prop.exists())  # not moved

    def test_files_on_passed_and_rewrites_refs(self):
        ok, msg = da.file_to_done(str(self.prop), {"verdict": "passed"})
        self.assertTrue(ok, msg)
        self.assertFalse(self.prop.exists())
        self.assertTrue((self.root / "docs/proposals/done/foo.md").exists())
        self.assertTrue((self.root / "docs/proposals/done/foo.plan.md").exists())
        # reference rewritten pending/ -> done/
        self.assertIn("done/foo.md", self.index.read_text())
        self.assertNotIn("pending/foo.md", self.index.read_text())


class Cli(unittest.TestCase):
    def test_eval_exit_code_and_json(self):
        with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as f:
            f.write(accept('- {id: 1, tier: mechanical, check: "true"}'))
            path = f.name
        rc = subprocess.run(["python3", str(SCRIPT), "eval", path],
                            capture_output=True, text=True)
        self.assertEqual(rc.returncode, 0)
        self.assertEqual(json.loads(rc.stdout)["verdict"], "passed")


if __name__ == "__main__":
    unittest.main()
