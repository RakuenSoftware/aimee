#!/usr/bin/env python3
"""Prove both raw identities are checked, independent of trailers and mailmap."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "scripts/check-commit-identities.py"
SPEC = importlib.util.spec_from_file_location("commit_identities", SOURCE)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class IdentityTests(unittest.TestCase):
    def test_reported_claude_identity_and_codex_defaults_are_rejected(self):
        for name, email in (("Claude", "noreply@anthropic.com"), ("Codex", "codex@openai.com"),
                            ("Operator", "noreply@anthropic.com"), ("Claude", "different@example.com"),
                            ("Operator", "123+copilot-swe-agent[bot]@users.noreply.github.com"),
                            ("cHaTgPt", "different@example.com")):
            with self.subTest(name=name, email=email):
                self.assertTrue(CHECKER.assistant_identity(name, email))

    def test_humans_and_non_ai_automation_are_not_misclassified(self):
        for name, email in (("Operator", "operator@example.com"), ("Claude Smith", "claude@example.com"),
                            ("github-actions[bot]", "41898282+github-actions[bot]@users.noreply.github.com"),
                            ("Alex", "alex@anthropic.com")):
            self.assertFalse(CHECKER.assistant_identity(name, email))

    def test_git_range_checks_author_and_committer_and_ignores_base_history(self):
        with tempfile.TemporaryDirectory() as directory:
            def git(*args, author="Operator", author_email="operator@example.com", committer="Operator", committer_email="operator@example.com"):
                env = dict(os.environ, GIT_AUTHOR_NAME=author, GIT_AUTHOR_EMAIL=author_email,
                           GIT_COMMITTER_NAME=committer, GIT_COMMITTER_EMAIL=committer_email)
                return subprocess.check_output(["git", "-C", directory, "-c", "core.hooksPath=/dev/null", *args], env=env, text=True).strip()
            git("init", "-q")
            git("commit", "-q", "--allow-empty", "-m", "Historical base", author="Claude", author_email="noreply@anthropic.com")
            base = git("rev-parse", "HEAD")
            git("commit", "-q", "--allow-empty", "-m", "Clean change")
            head = git("rev-parse", "HEAD")
            result = subprocess.run([sys.executable, str(SOURCE), base, head], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            git("commit", "-q", "--allow-empty", "-m", "No trailers here", committer="Codex", committer_email="codex@openai.com")
            head = git("rev-parse", "HEAD")
            result = subprocess.run([sys.executable, str(SOURCE), base, head], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("AI committer", result.stderr)
            git("commit", "-q", "--allow-empty", "-m", "Also no trailers", author="Claude", author_email="noreply@anthropic.com")
            Path(directory, ".mailmap").write_text("Operator <operator@example.com> Claude <noreply@anthropic.com>\n")
            head = git("rev-parse", "HEAD")
            result = subprocess.run([sys.executable, str(SOURCE), base, head], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("AI author", result.stderr)
            result = subprocess.run([sys.executable, str(SOURCE), "missing-ref", head], cwd=directory, capture_output=True)
            self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
