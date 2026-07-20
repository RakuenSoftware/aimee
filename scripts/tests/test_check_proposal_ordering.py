#!/usr/bin/env python3
"""Tests for the chronological Git proposal-ordering gate."""

from __future__ import annotations

import copy
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

    def test_claim_patterns_are_format_specific_literal_and_closed(self) -> None:
        yaml_patterns = ordering.claim_patterns("git-runtime-ready", ".yaml")
        json_patterns = ordering.claim_patterns("git-runtime-ready", ".json")
        yaml_accepted = (
            "git-runtime-ready: true",
            "  git-runtime-ready : true # approved",
        )
        yaml_rejected = (
            "git-runtime-ready: false",
            "git-runtime-ready: True",
            "git-runtime-ready: yes",
            "git-runtime-ready: !!bool true",
            "git-runtime-ready: true % comment",
            "git-runtime-ready: |",
            "Git-runtime-ready: true",
            "git-runtime-ready-extended: true",
            "prose git-runtime-ready: true",
            "# git-runtime-ready: true",
            '"git-runtime-ready": true,',
        )
        for line in yaml_accepted:
            with self.subTest(line=line):
                self.assertTrue(any(pattern.fullmatch(line) for pattern in yaml_patterns))
        for line in yaml_rejected:
            with self.subTest(line=line):
                self.assertFalse(any(pattern.fullmatch(line) for pattern in yaml_patterns))
        for line in ('"git-runtime-ready": true', '"git-runtime-ready": true,'):
            with self.subTest(line=line):
                self.assertTrue(any(pattern.fullmatch(line) for pattern in json_patterns))
        self.assertFalse(
            any(pattern.fullmatch("git-runtime-ready: true") for pattern in json_patterns)
        )

    def test_added_claim_ignores_deletion_context_and_binary(self) -> None:
        self.assertTrue(
            ordering.added_claim_signal(
                b"not ready\n", b"git-runtime-ready: true\n", "git-runtime-ready", ".yaml"
            )
        )
        self.assertFalse(
            ordering.added_claim_signal(
                b"git-runtime-ready: true\n", b"not ready\n", "git-runtime-ready", ".yaml"
            )
        )
        self.assertFalse(
            ordering.added_claim_signal(b"", b"\xff\x00", "git-runtime-ready", ".yaml")
        )

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
                root_claims=[],
            )
            self.assertEqual(len(signals), 2)
            self.assertTrue(signals[0][2][0][1].endswith("src/modules/git/example.c"))
        finally:
            tmp.cleanup()

    def test_history_records_claim_then_removal(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            doc = repo / "docs/modules/git.yaml"
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
                root_claims=[("docs/modules", "git-runtime-ready")],
            )
            self.assertEqual(len(signals), 1)
            self.assertEqual(
                signals[0][2],
                [("status-claim", "docs/modules:git-runtime-ready:docs/modules/git.yaml")],
            )
        finally:
            tmp.cleanup()

    def test_claim_remains_bound_to_its_declared_root(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            doc = repo / "docs/root-b/status.yaml"
            doc.parent.mkdir(parents=True)
            doc.write_text("claim-a: true\n", encoding="utf-8")
            head = self.commit(repo, "wrong-root claim")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths=set(),
                root_claims=[("docs/root-a", "claim-a"), ("docs/root-b", "claim-b")],
            )
            self.assertEqual(signals, [])
        finally:
            tmp.cleanup()

    def test_path_and_claim_are_distinct_signal_classes(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("signal\n", encoding="utf-8")
            claim = repo / "docs/modules/status.yaml"
            claim.parent.mkdir(parents=True)
            claim.write_text("git-runtime-ready: true\n", encoding="utf-8")
            head = self.commit(repo, "two signals")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths=set(),
                root_claims=[("docs/modules", "git-runtime-ready")],
            )
            self.assertEqual({item[0] for item in signals[0][2]}, {"path", "status-claim"})
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
                root_claims=[],
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
                root_claims=[],
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
                root_claims=[],
            )
            self.assertEqual(len(signals), 1)
            self.assertTrue(
                any("src/modules/git/example.c" in item[1] for item in signals[0][2])
            )
        finally:
            tmp.cleanup()

    def test_structured_claim_ignores_gitattributes_and_quoted_filename(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            attributes = repo / ".gitattributes"
            attributes.write_text("*.yaml -diff\n", encoding="utf-8")
            self.commit(repo, "disable yaml diff driver")
            doc = repo / 'docs/modules/weird\t"\nname.yaml'
            doc.parent.mkdir(parents=True)
            doc.write_text("git-runtime-ready: true\n", encoding="utf-8")
            head = self.commit(repo, "structured claim")
            signals = ordering.scan_history(
                repo,
                cutoff,
                head,
                exact_paths=set(),
                root_claims=[("docs/modules", "git-runtime-ready")],
            )
            claims = [
                evidence
                for _, _, entries in signals
                for kind, evidence in entries
                if kind == "status-claim"
            ]
            self.assertEqual(len(claims), 1)
            self.assertIn('weird\t"\nname.yaml', claims[0])
        finally:
            tmp.cleanup()

    def test_claim_rename_into_root_is_signal_and_rename_out_is_removal(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            outside = repo / "elsewhere/status.yaml"
            outside.parent.mkdir(parents=True)
            outside.write_text("git-runtime-ready: true\n", encoding="utf-8")
            source_commit = self.commit(repo, "outside claim")
            inside = repo / "docs/modules/status.yaml"
            inside.parent.mkdir(parents=True)
            outside.rename(inside)
            rename_in = self.commit(repo, "rename claim into root")
            inside.rename(outside)
            rename_out = self.commit(repo, "rename claim out of root")
            root_claims = [("docs/modules", "git-runtime-ready")]
            incoming = ordering.commit_signals(
                repo,
                source_commit,
                rename_in,
                exact_paths=set(),
                root_claims=root_claims,
            )
            outgoing = ordering.commit_signals(
                repo,
                rename_in,
                rename_out,
                exact_paths=set(),
                root_claims=root_claims,
            )
            self.assertTrue(any(kind == "status-claim" for kind, _ in incoming))
            self.assertFalse(any(kind == "status-claim" for kind, _ in outgoing))
            self.assertTrue(ordering.is_ancestor(repo, cutoff, rename_out))
        finally:
            tmp.cleanup()

    def test_complete_dag_scan_keeps_side_branch_signal_and_revert(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            anchor_file = repo / "contract.md"
            anchor_file.write_text("approved\n", encoding="utf-8")
            anchor = self.commit(repo, "anchor")
            self.git(repo, "switch", "-q", "-c", "side")
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("signal\n", encoding="utf-8")
            self.commit(repo, "side signal")
            source.unlink()
            self.commit(repo, "side revert")
            self.git(repo, "switch", "-q", "master")
            self.git(
                repo,
                "-c",
                "user.name=Aimee Test",
                "-c",
                "user.email=aimee@example.invalid",
                "merge",
                "--no-ff",
                "-m",
                "merge side",
                "side",
            )
            head = self.git(repo, "rev-parse", "HEAD")
            signals = ordering.scan_history(
                repo, cutoff, head, exact_paths=set(), root_claims=[]
            )
            self.assertEqual(len(signals), 2)
            ordering.enforce_signal_precedence(repo, anchor, signals)
        finally:
            tmp.cleanup()

    def test_pr_shape_branch_without_anchor_fails_precedence(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            self.git(repo, "switch", "-q", "-c", "proposed")
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("signal\n", encoding="utf-8")
            self.commit(repo, "pre-anchor signal")
            self.git(repo, "switch", "-q", "master")
            anchor_file = repo / "contract.md"
            anchor_file.write_text("approved\n", encoding="utf-8")
            anchor = self.commit(repo, "anchor")
            self.git(
                repo,
                "-c",
                "user.name=Aimee Test",
                "-c",
                "user.email=aimee@example.invalid",
                "merge",
                "--no-ff",
                "-m",
                "synthetic merge",
                "proposed",
            )
            head = self.git(repo, "rev-parse", "HEAD")
            signals = ordering.scan_history(
                repo, cutoff, head, exact_paths=set(), root_claims=[]
            )
            with self.assertRaisesRegex(ordering.OrderingError, "git-contract-ordering"):
                ordering.enforce_signal_precedence(repo, anchor, signals)
        finally:
            tmp.cleanup()

    def test_all_signal_evidence_is_rendered_on_failure(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            with self.assertRaises(ordering.OrderingError) as caught:
                ordering.enforce_signal_precedence(
                    repo,
                    "f" * 40,
                    [(cutoff, cutoff, [("path", "one"), ("status-claim", "two")])],
                )
            self.assertIn("path:one", str(caught.exception))
            self.assertIn("status-claim:two", str(caught.exception))
        finally:
            tmp.cleanup()

    def test_event_binding(self) -> None:
        head = "a" * 40
        with mock.patch.dict(os.environ, {}, clear=True):
            ordering.validate_event(head)
        with mock.patch.dict(
            os.environ,
            {
                "GITHUB_EVENT_NAME": "pull_request",
                "GITHUB_REF": "refs/pull/123/merge",
                "GITHUB_SHA": head,
                "GITHUB_BASE_REF": "feature/core-modularization",
                "GITHUB_HEAD_REF": "slice/example",
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
        for missing in ("GITHUB_SHA", "GITHUB_REF"):
            env = {
                "GITHUB_EVENT_NAME": "push",
                "GITHUB_REF": "refs/heads/feature/core-modularization",
                "GITHUB_SHA": head,
            }
            env.pop(missing)
            expected = "event-head" if missing == "GITHUB_SHA" else "event-ref"
            with (
                self.subTest(missing=missing),
                mock.patch.dict(os.environ, env, clear=True),
                self.assertRaisesRegex(ordering.OrderingError, expected),
            ):
                ordering.validate_event(head)

        partials = (
            {"GITHUB_SHA": head},
            {"GITHUB_REF": "refs/heads/feature/core-modularization"},
            {"GITHUB_BASE_REF": "feature/core-modularization"},
        )
        for env in partials:
            with (
                self.subTest(env=env),
                mock.patch.dict(os.environ, env, clear=True),
                self.assertRaisesRegex(ordering.OrderingError, "event-"),
            ):
                ordering.validate_event(head)
        wrong_base = {
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_REF": "refs/pull/123/merge",
            "GITHUB_SHA": head,
            "GITHUB_BASE_REF": "main",
            "GITHUB_HEAD_REF": "slice/example",
        }
        with mock.patch.dict(os.environ, wrong_base, clear=True), self.assertRaisesRegex(
            ordering.OrderingError, "event-base"
        ):
            ordering.validate_event(head)

    def test_live_input_rejects_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            target = repo / "target.md"
            target.write_text("pass\n", encoding="utf-8")
            link = repo / "link.md"
            link.symlink_to(target)
            with self.assertRaisesRegex(ordering.OrderingError, "input-symlink"):
                ordering.read_live_text(repo, "link.md")

    def test_git_execution_failure_is_fail_closed(self) -> None:
        with mock.patch.object(ordering, "GIT", "/definitely/missing/git"), self.assertRaisesRegex(
            ordering.OrderingError, "git-exec"
        ):
            ordering.git(REPO_ROOT, "status")

    def test_live_discovery_equals_immutable_slice2_blob(self) -> None:
        pinned, pinned_handoff = ordering.canonical_metadata(REPO_ROOT)
        live, live_handoff = ordering.live_metadata(REPO_ROOT)
        ordering.validate_discovery(live, pinned, live_handoff, pinned_handoff)

        drifted_handoff = copy.deepcopy(pinned_handoff)
        drifted_handoff["receiver"] = "changed"
        with self.assertRaisesRegex(ordering.OrderingError, "discovery-drift"):
            ordering.validate_discovery(live, pinned, drifted_handoff, pinned_handoff)

        drifted_contract = copy.deepcopy(live)
        drifted_contract["trigger_surface"]["status_claim_roots"].append(
            {"path": "docs/extra", "claim": "git-runtime-ready"}
        )
        with self.assertRaisesRegex(ordering.OrderingError, "discovery-drift"):
            ordering.validate_discovery(
                drifted_contract, pinned, live_handoff, pinned_handoff
            )

    def test_contract_shape_failure_has_stable_rule(self) -> None:
        contract = {"trigger_surface": {group: [] for group in ordering.TRIGGER_GROUPS}}
        del contract["trigger_surface"]["readiness_markers"]
        with self.assertRaisesRegex(ordering.OrderingError, "contract-shape"):
            ordering.path_metadata(contract)

    def test_non_repository_config_root_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(ordering.main(["--config-root", tmp]), 1)


if __name__ == "__main__":
    unittest.main()
