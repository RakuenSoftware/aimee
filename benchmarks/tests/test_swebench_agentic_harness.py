#!/usr/bin/env python3
"""Unit tests for S1 — the per-worker agentic harness pure core.

No live server, docker, or network: provisioning + patch extraction run against a local temp
git fixture; the resource table, secret scan, and loop bounds are pure. Mirrors S0's strictness
— every reproducibility-critical behaviour has a test, and the roundtable's Q5 hygiene rulings
are each pinned by a case.
Run: python3 -m unittest benchmarks.tests.test_swebench_agentic_harness
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from benchmarks.coding import swebench_agentic_harness as H


def _git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True,
                          capture_output=True, text=True)


def _make_repo(root: Path) -> str:
    """A tiny git repo with one tracked file + a .gitignore, committed. Returns base_commit."""
    root.mkdir(parents=True, exist_ok=True)
    _git(root, "init", "-q")
    _git(root, "config", "user.email", "t@t")
    _git(root, "config", "user.name", "t")
    (root / "mod.py").write_text("def f():\n    return 1\n")
    (root / ".gitignore").write_text("build/\n*.pyc\n")
    _git(root, "add", "-A")
    _git(root, "commit", "-qm", "base")
    return _git(root, "rev-parse", "HEAD").stdout.strip()


class TestEnvAllocator(unittest.TestCase):
    def test_deterministic_same_key_same_env(self):
        a = H.EnvAllocator("/r")
        e1 = a.allocate("inst", "C", 0)
        e2 = a.allocate("inst", "C", 0)
        self.assertEqual(e1, e2)

    def test_port_ranges_non_overlapping(self):
        a = H.EnvAllocator("/r")
        for i in range(5):
            a.allocate("inst", "C", i)
        ranges = a.port_ranges()
        for (s1, e1), (s2, e2) in zip(ranges, ranges[1:]):
            self.assertLess(e1, s2, f"ranges overlap: {(s1,e1)} vs {(s2,e2)}")

    def test_release_frees_slot_for_reuse(self):
        a = H.EnvAllocator("/r", max_workers=2)
        a.allocate("i", "C", 0)
        a.allocate("i", "C", 1)
        with self.assertRaises(RuntimeError):
            a.allocate("i", "C", 2)  # pool exhausted
        a.release("i", "C", 0)
        # now a fresh key can be allocated again
        e = a.allocate("i", "C", 2)
        self.assertIsInstance(e, H.WorkerEnv)

    def test_env_dict_isolates_home_tmp_cache(self):
        e = H.EnvAllocator("/r").allocate("i", "A", 0)
        env = e.env()
        self.assertTrue(env["TMPDIR"].endswith("tmp"))
        self.assertEqual(env["XDG_CACHE_HOME"], env["PIP_CACHE_DIR"])
        self.assertNotEqual(env["HOME"], env["TMPDIR"])


class TestResolveTestCommand(unittest.TestCase):
    def test_known_repos(self):
        self.assertIn("runtests.py", H.resolve_test_command("django/django"))
        self.assertIn("pytest", H.resolve_test_command("sympy/sympy"))

    def test_unknown_repo_is_none(self):
        self.assertIsNone(H.resolve_test_command("acme/widget"))


class TestProvisionAndPatch(unittest.TestCase):
    def test_provision_checks_out_base_commit_detached(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            base = _make_repo(root / "repo")
            ws = H.provision_workspace(str(root / "repo"), base, str(root / "ws"))
            head = _git(ws, "rev-parse", "HEAD").stdout.strip()
            self.assertEqual(head, base)

    def test_patch_includes_new_untracked_source_excludes_ignored(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            base = _make_repo(root / "repo")
            ws = H.provision_workspace(str(root / "repo"), base, str(root / "ws"))
            # agent edits tracked file, creates a NEW source file, and a build artifact (ignored)
            (Path(ws) / "mod.py").write_text("def f():\n    return 2\n")
            (Path(ws) / "new_mod.py").write_text("X = 1\n")
            (Path(ws) / "build").mkdir()
            (Path(ws) / "build" / "junk.o").write_text("artifact")
            patch, n = H.extract_patch(ws, base)
            self.assertIn("mod.py", patch)
            self.assertIn("new_mod.py", patch)      # new source IS in the patch
            self.assertNotIn("junk.o", patch)       # ignored artifact is NOT
            self.assertIn("return 2", patch)

    def test_patch_is_deterministic(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            base = _make_repo(root / "repo")
            ws = H.provision_workspace(str(root / "repo"), base, str(root / "ws"))
            (Path(ws) / "mod.py").write_text("def f():\n    return 2\n")
            p1, _ = H.extract_patch(ws, base)
            p2, _ = H.extract_patch(ws, base)
            self.assertEqual(p1, p2)

    def test_in_loop_commit_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            base = _make_repo(root / "repo")
            ws = H.provision_workspace(str(root / "repo"), base, str(root / "ws"))
            (Path(ws) / "mod.py").write_text("def f():\n    return 2\n")
            _git(ws, "add", "-A")
            _git(ws, "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "oops")
            with self.assertRaises(H.PatchError):
                H.extract_patch(ws, base)

    def test_submodule_repo_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            _make_repo(root / "repo")
            (root / "repo" / ".gitmodules").write_text("[submodule \"x\"]\n")
            with self.assertRaises(H.ProvisionError):
                H.provision_workspace(str(root / "repo"), "HEAD", str(root / "ws"))


class TestSecretScan(unittest.TestCase):
    def test_redacts_aws_and_private_key(self):
        text = ("+AKIAIOSFODNN7EXAMPLE\n"
                "+-----BEGIN RSA PRIVATE KEY-----\n")
        out, n = H.scan_and_redact_secrets(text)
        self.assertGreaterEqual(n, 2)
        self.assertNotIn("AKIAIOSFODNN7EXAMPLE", out)
        self.assertIn(H._REDACTED, out)

    def test_redacts_high_entropy_assignment(self):
        out, n = H.scan_and_redact_secrets('api_key = "aB3xZ9qL7mN2pQ8rT5vW"\n')
        self.assertEqual(n, 1)
        self.assertNotIn("aB3xZ9qL7mN2pQ8rT5vW", out)

    def test_leaves_ordinary_code_alone(self):
        code = "def total(items):\n    return sum(items)\n"
        out, n = H.scan_and_redact_secrets(code)
        self.assertEqual(n, 0)
        self.assertEqual(out, code)

    def test_redaction_is_stable_byte_for_byte(self):
        text = "+token = 'aB3xZ9qL7mN2pQ8rT5vW'\n"
        self.assertEqual(H.scan_and_redact_secrets(text), H.scan_and_redact_secrets(text))


class TestLoopBudget(unittest.TestCase):
    def test_stops_on_verify_pass(self):
        b = H.LoopBudget()
        self.assertFalse(b.should_continue(turns=1, wall_s=1, usd=0, verify_passed=True))
        self.assertEqual(b.stop_reason(turns=1, wall_s=1, usd=0, verify_passed=True), "verify_passed")

    def test_stops_on_max_turns(self):
        b = H.LoopBudget(max_turns=3)
        self.assertFalse(b.should_continue(turns=3, wall_s=1, usd=0, verify_passed=False))
        self.assertEqual(b.stop_reason(turns=3, wall_s=1, usd=0, verify_passed=False), "max_turns")

    def test_continues_within_bounds(self):
        b = H.LoopBudget(max_turns=5, max_wall_s=100, max_usd=1)
        self.assertTrue(b.should_continue(turns=2, wall_s=10, usd=0.1, verify_passed=False))


class TestFingerprint(unittest.TestCase):
    def test_same_inputs_same_id(self):
        a = H.workspace_fingerprint("r", "c0ffee", "patch body")
        b = H.workspace_fingerprint("r", "c0ffee", "patch body")
        self.assertEqual(a, b)

    def test_different_patch_different_id(self):
        self.assertNotEqual(H.workspace_fingerprint("r", "c", "x"),
                            H.workspace_fingerprint("r", "c", "y"))


class TestAgenticPromptAndDiff(unittest.TestCase):
    def test_prompt_includes_issue_and_diff_instruction(self):
        p = H.agentic_prompt({"repo": "django/django", "problem": "TZ bug",
                              "file": "django/utils/tz.py", "region": "def now(): pass"}, arm="C")
        self.assertIn("TZ bug", p)
        self.assertIn("django/utils/tz.py", p)
        self.assertIn("```diff", p)

    def test_extract_diff_from_fenced_block(self):
        txt = "prose\n```diff\ndiff --git a/x b/x\n+ok\n```\ntrailing"
        self.assertEqual(H.extract_diff_from_text(txt), "diff --git a/x b/x\n+ok")

    def test_extract_diff_stops_at_prose(self):
        txt = "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n+ok\nNow some prose."
        out = H.extract_diff_from_text(txt)
        self.assertIn("+ok", out)
        self.assertNotIn("prose", out)

    def test_worktree_branch_is_deterministic_and_safe(self):
        b = H.worktree_branch("django__django-12908", "C", 2)
        self.assertEqual(b, H.worktree_branch("django__django-12908", "C", 2))
        self.assertTrue(b.startswith("aimee/wi/swebench-"))
        self.assertNotIn(" ", b)


class TestRunAgenticLoopLive(unittest.TestCase):
    def test_loop_extracts_patch_from_returned_diff(self):
        from benchmarks.coding import swebench_live_transport as LT

        def fake_dispatch(role, prompt, **kw):
            self.assertEqual(role, "code")
            self.assertTrue(kw["tools"] and kw["worktree"])   # FINDING 4
            return LT.DispatchOutcome(job_id=9, status="done",
                                      result="```diff\ndiff --git a/f b/f\n+patch\n```",
                                      agent_name="GLM-5.2", delegation_id="deleg-1-2-9",
                                      api_calls=3, polls=1)

        env = H.EnvAllocator("/tmp/wsx").allocate("i", "C", 0)
        res = H.run_agentic_loop({"instance_id": "i", "repo": "x/y", "base_commit": "0" * 40,
                                  "problem": "b"}, env, H.LoopBudget(), arm="C", worker="GLM-5.2",
                                 base_repo="", dispatch=fake_dispatch)
        self.assertTrue(res.ok)
        self.assertIn("+patch", res.patch)
        self.assertEqual(res.job_id, 9)
        self.assertEqual(res.delegation_id, "deleg-1-2-9")

    def test_failed_dispatch_is_recorded_not_raised(self):
        from benchmarks.coding import swebench_live_transport as LT
        fake = lambda role, prompt, **kw: LT.DispatchOutcome(None, "error", "", "", None, 0, 0,
                                                             error="no job_id")
        env = H.EnvAllocator("/tmp/wsx").allocate("i", "A", 0)
        res = H.run_agentic_loop({"instance_id": "i", "base_commit": "0" * 40}, env,
                                 H.LoopBudget(), arm="A", dispatch=fake)
        self.assertFalse(res.ok)
        self.assertEqual(res.status, "error")

    def test_response_text_patch_is_non_authoritative(self):
        # Dev mode (no co-located workspace): a returned diff is usable but NOT graded-authoritative
        # (it is only a claim — the roundtable hallucination-gap guard).
        from benchmarks.coding import swebench_live_transport as LT
        fake = lambda role, prompt, **kw: LT.DispatchOutcome(9, "done",
            "```diff\ndiff --git a/f b/f\n+claimed\n```", "GLM-5.2", "d-9", 1, 1)
        env = H.EnvAllocator("/tmp/wsx").allocate("i", "C", 0)
        res = H.run_agentic_loop({"instance_id": "i", "base_commit": "0" * 40, "problem": "b"},
                                 env, H.LoopBudget(), arm="C", worker="GLM-5.2", base_repo="",
                                 dispatch=fake)
        self.assertEqual(res.patch_source, "response_text")
        self.assertFalse(res.authoritative)          # never graded from a mere claim
        self.assertTrue(res.ok)                       # still a produced patch (dev/telemetry)


class TestHallucinationGapGuard(unittest.TestCase):
    """Co-located mode: the graded patch is the ACTUAL filesystem diff (git diff), never the diff
    the delegate merely CLAIMED in its response text."""

    def _base_repo(self):
        import subprocess, tempfile
        d = tempfile.mkdtemp()
        run = lambda *a: subprocess.run(["git", "-C", d, *a], check=True, capture_output=True,
                                        text=True, env={"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL":
                                        "t@t", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL":
                                        "t@t", "PATH": os.environ.get("PATH", "")})
        subprocess.run(["git", "init", "-q", d], check=True, capture_output=True)
        Path(d, "f.py").write_text("x = 1\n")
        run("add", "-A"); run("-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "base")
        commit = run("rev-parse", "HEAD").stdout.strip()
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        return d, commit

    def test_colocated_uses_workspace_diff_not_claim(self):
        from benchmarks.coding import swebench_live_transport as LT
        base, commit = self._base_repo()
        env = H.EnvAllocator(__import__("tempfile").mkdtemp()).allocate("i", "A", 0)

        def fake_dispatch(role, prompt, **kw):
            # The agent ACTUALLY edits the workspace...
            Path(env.workspace, "f.py").write_text("x = 2  # real edit\n")
            # ...but its response text CLAIMS a different, unrelated diff.
            return LT.DispatchOutcome(5, "done", "```diff\ndiff --git a/OTHER b/OTHER\n+lie\n```",
                                      "codex", "d-5", 2, 1)

        res = H.run_agentic_loop({"instance_id": "i", "repo": "x/y", "base_commit": commit},
                                 env, H.LoopBudget(), arm="A", base_repo=base, dispatch=fake_dispatch)
        self.assertEqual(res.patch_source, "workspace")
        self.assertTrue(res.authoritative)
        self.assertIn("real edit", res.patch)        # the FS edit, graded
        self.assertNotIn("lie", res.patch)           # the claimed diff is NOT graded
        self.assertIn("OTHER", res.returned_diff)    # claim retained only as diagnostic

    def test_colocated_clean_workspace_is_not_graded_from_claim(self):
        from benchmarks.coding import swebench_live_transport as LT
        base, commit = self._base_repo()
        env = H.EnvAllocator(__import__("tempfile").mkdtemp()).allocate("i", "A", 0)
        # The agent edits NOTHING but claims a fix -> must NOT be graded (patch_source none).
        fake = lambda role, prompt, **kw: LT.DispatchOutcome(6, "done",
            "```diff\ndiff --git a/f.py b/f.py\n+claimed fix\n```", "codex", "d-6", 1, 1)
        res = H.run_agentic_loop({"instance_id": "i", "repo": "x/y", "base_commit": commit},
                                 env, H.LoopBudget(), arm="A", base_repo=base, dispatch=fake)
        self.assertEqual(res.patch_source, "none")
        self.assertFalse(res.authoritative)
        self.assertEqual(res.patch, "")


if __name__ == "__main__":
    unittest.main()
