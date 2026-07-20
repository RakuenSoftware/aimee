#!/usr/bin/env python3
"""Tests for the chronological Git proposal-ordering gate."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO_ROOT / "scripts/check_proposal_ordering.py"
SPEC = importlib.util.spec_from_file_location("check_proposal_ordering", CHECKER_PATH)
assert SPEC and SPEC.loader
ordering = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ordering)


class ProposalOrderingTests(unittest.TestCase):
    def git(self, repo: Path, *args: str) -> str:
        result = subprocess.run(
            [ordering.GIT, *args],
            cwd=repo,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        return result.stdout.strip()

    def commit(self, repo: Path, message: str) -> str:
        self.git(repo, "add", "-A")
        self.git(
            repo,
            "-c",
            "user.name=Aimee Test",
            "-c",
            "user.email=aimee@example.invalid",
            "commit",
            "-m",
            message,
        )
        return self.git(repo, "rev-parse", "HEAD")

    def make_repo(self) -> tuple[tempfile.TemporaryDirectory[str], Path, str]:
        tmp = tempfile.TemporaryDirectory()
        repo = Path(tmp.name)
        self.git(repo, "init", "-q")
        (repo / "README.md").write_text("base\n", encoding="utf-8")
        cutoff = self.commit(repo, "base")
        return tmp, repo, cutoff

    def test_current_repository_passes(self) -> None:
        self.assertEqual(ordering.validate_ordering(REPO_ROOT), 0)

    def test_name_status_parses_all_path_shapes(self) -> None:
        raw = b"M\0one\0R100\0old\0new\0C075\0source\0copy\0D\0gone\0"
        self.assertEqual(
            ordering.parse_name_status(raw),
            [
                ("M", ("one",)),
                ("R100", ("old", "new")),
                ("C075", ("source", "copy")),
                ("D", ("gone",)),
            ],
        )

    def test_source_and_exact_path_detection(self) -> None:
        exact = {"src/modules/git/module.yaml", "src/generated/modules.mk"}
        self.assertTrue(ordering.source_or_exact_signal("src/modules/git/a.c", exact))
        self.assertTrue(ordering.source_or_exact_signal("src/generated/modules.mk", exact))
        self.assertFalse(ordering.source_or_exact_signal("src/modules/memory/a.c", exact))

    def test_claim_patterns_are_closed_whole_line_and_case_sensitive(self) -> None:
        patterns = ordering.claim_patterns({"git-runtime-ready"})
        accepted = (
            "git-runtime-ready: true",
            "  git-runtime-ready : true # approved",
            '"git-runtime-ready": true,',
        )
        rejected = (
            "git-runtime-ready: false",
            "Git-runtime-ready: true",
            "git-runtime-ready-extended: true",
            "prose git-runtime-ready: true",
            "# git-runtime-ready: true",
        )
        for line in accepted:
            with self.subTest(line=line):
                self.assertTrue(any(pattern.fullmatch(line) for pattern in patterns))
        for line in rejected:
            with self.subTest(line=line):
                self.assertFalse(any(pattern.fullmatch(line) for pattern in patterns))

    def test_added_claim_ignores_deletion_context_and_binary(self) -> None:
        patterns = ordering.claim_patterns({"git-runtime-ready"})
        self.assertTrue(
            ordering.added_claim_signal(b"+git-runtime-ready: true\n", patterns)
        )
        self.assertFalse(
            ordering.added_claim_signal(b"-git-runtime-ready: true\n", patterns)
        )
        self.assertFalse(ordering.added_claim_signal(b"\xff\x00", patterns))

    def test_history_records_source_signal_even_after_revert(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("one\n", encoding="utf-8")
            self.commit(repo, "source signal")
            source.unlink()
            head = self.commit(repo, "revert source signal")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths=set(),
                status_roots=set(),
                claims=set(),
            )
            self.assertEqual(len(signals), 2)
            self.assertTrue(signals[0][2][0].endswith("src/modules/git/example.c"))
        finally:
            tmp.cleanup()

    def test_history_records_claim_then_removal(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            doc = repo / "docs/modules/git.md"
            doc.parent.mkdir(parents=True)
            doc.write_text("git-runtime-ready: true\n", encoding="utf-8")
            self.commit(repo, "claim")
            doc.write_text("not ready\n", encoding="utf-8")
            head = self.commit(repo, "remove claim")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths=set(),
                status_roots={"docs/modules"},
                claims={"git-runtime-ready"},
            )
            self.assertEqual(len(signals), 1)
            self.assertEqual(signals[0][2], ["status-claim"])
        finally:
            tmp.cleanup()

    def test_exact_descriptor_path_triggers_without_source_change(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            descriptor = repo / "src/modules/git/module.yaml"
            descriptor.parent.mkdir(parents=True)
            descriptor.write_text("module: git\n", encoding="utf-8")
            head = self.commit(repo, "descriptor")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths={"src/modules/git/module.yaml"},
                status_roots=set(),
                claims=set(),
            )
            self.assertEqual(len(signals), 1)
        finally:
            tmp.cleanup()

    def test_signal_must_follow_anchor_strictly(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            anchor_file = repo / "contract.md"
            anchor_file.write_text("approved\n", encoding="utf-8")
            anchor = self.commit(repo, "anchor")
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("signal\n", encoding="utf-8")
            head = self.commit(repo, "signal after anchor")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths=set(),
                status_roots=set(),
                claims=set(),
            )
            ordering.enforce_signal_precedence(repo, anchor, signals)
            with self.assertRaisesRegex(ordering.OrderingError, "git-contract-ordering"):
                ordering.enforce_signal_precedence(repo, head, signals)
        finally:
            tmp.cleanup()

    def test_rename_out_of_historical_git_tree_triggers(self) -> None:
        tmp, repo, _ = self.make_repo()
        try:
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("historical\n", encoding="utf-8")
            cutoff = self.commit(repo, "historical git source")
            destination = repo / "src/elsewhere/example.c"
            destination.parent.mkdir(parents=True)
            source.rename(destination)
            head = self.commit(repo, "rename out")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths=set(),
                status_roots=set(),
                claims=set(),
            )
            self.assertEqual(len(signals), 1)
            self.assertTrue(any("src/modules/git/example.c" in item for item in signals[0][2]))
        finally:
            tmp.cleanup()

    def test_event_binding(self) -> None:
        head = "a" * 40
        with mock.patch.dict(os.environ, {}, clear=True):
            ordering.validate_event(head)
        with mock.patch.dict(
            os.environ,
            {
                "GITHUB_EVENT_NAME": "push",
                "GITHUB_REF": "refs/heads/feature/core-modularization",
                "GITHUB_SHA": head,
            },
            clear=True,
        ):
            ordering.validate_event(head)
        with mock.patch.dict(
            os.environ,
            {"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/other", "GITHUB_SHA": head},
            clear=True,
        ), self.assertRaisesRegex(ordering.OrderingError, "event-ref"):
            ordering.validate_event(head)

    def test_live_discovery_equals_immutable_slice2_blob(self) -> None:
        contract_checker = ordering.load_contract_checker()
        pinned = ordering.canonical_contract(REPO_ROOT, contract_checker)
        live = contract_checker.load_validated_contract(
            REPO_ROOT, require_status="roundtable-approved", check_git=True
        )
        self.assertEqual(ordering.discovery_view(live), ordering.discovery_view(pinned))


if __name__ == "__main__":
    unittest.main()
