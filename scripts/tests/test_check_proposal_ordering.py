#!/usr/bin/env python3
"""Tests for the chronological Git proposal-ordering gate."""

from __future__ import annotations

import copy
import importlib.util
import os
import re
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

    def merge(self, repo: Path, branch: str, message: str) -> str:
        self.git(
            repo,
            "-c",
            "user.name=Aimee Test",
            "-c",
            "user.email=aimee@example.invalid",
            "merge",
            "--no-ff",
            "-m",
            message,
            branch,
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
        # Gated on ancestry, matching the rule itself. Gating on first-parent
        # membership skipped this on every ordinary checkout -- including the
        # integration branch -- so the one test that exercises the real history
        # never ran.
        head = ordering.git_text(REPO_ROOT, "rev-parse", "HEAD")
        if not ordering.descends_from(REPO_ROOT, ordering.SLICE2_ANCHOR, head):
            self.skipTest("checkout does not descend from the approved Slice 2 anchor")
        signal_count = ordering.validate_ordering(REPO_ROOT)
        self.assertIsInstance(signal_count, int)
        self.assertGreaterEqual(signal_count, 1)

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

    def test_name_status_preserves_non_utf8_git_paths(self) -> None:
        self.assertEqual(
            ordering.parse_name_status(b"M\0bad-\xff.yml\0"),
            [("M", ("bad-\udcff.yml",))],
        )

    def test_source_and_exact_path_detection(self) -> None:
        exact = {"src/modules/git/module.yaml", "src/generated/modules.mk"}
        self.assertTrue(ordering.source_or_exact_signal("src/modules/git", exact))
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
            ordering.claim_signal_in_diff(
                b"not ready\n", b"git-runtime-ready: true\n", "git-runtime-ready", ".yaml"
            )
        )
        self.assertFalse(
            ordering.claim_signal_in_diff(
                b"git-runtime-ready: true\n", b"not ready\n", "git-runtime-ready", ".yaml"
            )
        )
        self.assertFalse(
            ordering.claim_signal_in_diff(b"", b"\xff\x00", "git-runtime-ready", ".yaml")
        )

    def test_claim_toggle_and_yaml_block_scalars(self) -> None:
        self.assertTrue(
            ordering.claim_signal_in_diff(
                b"git-runtime-ready: false\n",
                b"git-runtime-ready: true\n",
                "git-runtime-ready",
                ".yaml",
            )
        )
        for marker in ("|", ">-"):
            after = f"description: {marker}\n  git-runtime-ready: true\nnext: value\n"
            with self.subTest(marker=marker):
                self.assertFalse(
                    ordering.claim_signal_in_diff(
                        b"", after.encode(), "git-runtime-ready", ".yaml"
                    )
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
            spacer = repo / "spacer"
            spacer.write_text("separate approval from signal\n", encoding="utf-8")
            self.commit(repo, "post-anchor boundary")
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
            direct = [(head, anchor, [("path", "M:src/modules/git/example.c")])]
            with self.assertRaisesRegex(ordering.OrderingError, "git-contract-ordering"):
                ordering.enforce_signal_precedence(repo, anchor, direct)
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

    def test_exact_git_tree_root_gitlink_is_signal(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            self.git(
                repo,
                "update-index",
                "--add",
                "--cacheinfo",
                f"160000,{cutoff},src/modules/git",
            )
            self.git(
                repo,
                "-c",
                "user.name=Aimee Test",
                "-c",
                "user.email=aimee@example.invalid",
                "commit",
                "-m",
                "git root gitlink",
            )
            head = self.git(repo, "rev-parse", "HEAD")
            signals = ordering.scan_history(
                repo, cutoff, head, exact_paths=set(), root_claims=[]
            )
            self.assertEqual(len(signals), 1)
            self.assertIn("src/modules/git", signals[0][2][0][1])
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

    def test_copy_of_in_root_claim_uses_empty_destination_baseline(self) -> None:
        tmp, repo, _ = self.make_repo()
        try:
            source = repo / "docs/modules/source.yaml"
            source.parent.mkdir(parents=True)
            source.write_text("git-runtime-ready: true\n", encoding="utf-8")
            parent = self.commit(repo, "source claim")
            destination = repo / "docs/modules/copy.yaml"
            destination.write_bytes(source.read_bytes())
            commit = self.commit(repo, "copy claim")
            signals = ordering.commit_signals(
                repo,
                parent,
                commit,
                exact_paths=set(),
                root_claims=[("docs/modules", "git-runtime-ready")],
            )
            self.assertTrue(any(kind == "status-claim" for kind, _ in signals))
        finally:
            tmp.cleanup()

    def test_complete_dag_scan_keeps_side_branch_signal_and_revert(self) -> None:
        tmp, repo, cutoff = self.make_repo()
        try:
            anchor_file = repo / "contract.md"
            anchor_file.write_text("approved\n", encoding="utf-8")
            anchor = self.commit(repo, "anchor")
            spacer = repo / "spacer"
            spacer.write_text("post-anchor boundary\n", encoding="utf-8")
            self.commit(repo, "post-anchor boundary")
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

    def test_batched_history_agrees_with_the_per_commit_diff(self) -> None:
        """The one-pass log must report what per-commit diffing reported.

        commit_signals is still the reference: it asks git the direct question.
        batched_history is an optimisation, so the two must not drift -- a merge,
        a rename, a copy and a delete all take different paths through the -z
        stream, so the fixture exercises each.
        """
        tmp, repo, cutoff = self.make_repo()
        try:
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("one\n", encoding="utf-8")
            (repo / "doomed.txt").write_text("delete me\n", encoding="utf-8")
            self.commit(repo, "add sources")
            self.git(repo, "checkout", "-b", "side")
            source.write_text("two\n", encoding="utf-8")
            self.commit(repo, "edit on a side branch")
            self.git(repo, "checkout", "-")
            self.git(repo, "mv", "src/modules/git/example.c", "src/modules/git/renamed.c")
            self.commit(repo, "rename")
            (repo / "doomed.txt").unlink()
            self.commit(repo, "delete")
            self.merge(repo, "side", "merge side")
            head = self.git(repo, "rev-parse", "HEAD")

            batched = ordering.batched_history(repo, cutoff, head)
            self.assertTrue(batched)
            for commit, (parent, records) in batched.items():
                with self.subTest(commit=commit[:10]):
                    self.assertEqual(parent, ordering.first_parent(repo, commit))
                    reference = ordering.parse_name_status(
                        ordering.git(
                            repo, "diff", "--name-status", "-z", "-M", "-C",
                            "--find-copies-harder", parent, commit, "--",
                        )
                    )
                    self.assertEqual(records, reference)
        finally:
            tmp.cleanup()

    def test_a_signal_is_accepted_when_the_anchor_arrived_by_merge(self) -> None:
        """The integration branch's actual shape must pass.

        The approval lands on its own branch and the integration branch merges
        it, so the anchor is a second parent and is on no commit's first-parent
        ancestry there. Requiring first-parent membership therefore rejected
        every post-approval Git-module change on `testing` -- 91 of 95 signals
        -- while passing on the branch the anchor happened to sit on.
        """
        tmp, repo, cutoff = self.make_repo()
        try:
            integration = self.git(repo, "rev-parse", "--abbrev-ref", "HEAD")
            self.git(repo, "checkout", "-b", "approval")
            (repo / "contract.md").write_text("approved\n", encoding="utf-8")
            anchor = self.commit(repo, "anchor")
            self.git(repo, "checkout", integration)
            self.merge(repo, "approval", "merge approval")
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("signal\n", encoding="utf-8")
            self.commit(repo, "signal after approval")
            head = self.git(repo, "rev-parse", "HEAD")

            signals = ordering.scan_history(
                repo, cutoff, head, exact_paths=set(), root_claims=[]
            )
            self.assertTrue(signals)
            # Red before green: the retired rule rejects exactly this shape.
            for _, parent, _ in signals:
                self.assertFalse(ordering.on_first_parent_chain(repo, anchor, parent))
            ordering.enforce_signal_precedence(repo, anchor, signals)
        finally:
            tmp.cleanup()

    def test_a_signal_that_predates_the_anchor_is_still_rejected(self) -> None:
        """Waiving the frozen ones must not stop the rule catching new ones."""
        tmp, repo, cutoff = self.make_repo()
        try:
            source = repo / "src/modules/git/example.c"
            source.parent.mkdir(parents=True)
            source.write_text("pre-approval\n", encoding="utf-8")
            offender = self.commit(repo, "signal before approval")
            (repo / "contract.md").write_text("approved\n", encoding="utf-8")
            anchor = self.commit(repo, "anchor")
            head = self.git(repo, "rev-parse", "HEAD")
            signals = ordering.scan_history(
                repo, cutoff, head, exact_paths=set(), root_claims=[]
            )
            self.assertTrue(any(commit == offender for commit, _, _ in signals))
            with self.assertRaisesRegex(ordering.OrderingError, "git-contract-ordering"):
                ordering.enforce_signal_precedence(repo, anchor, signals)
        finally:
            tmp.cleanup()

    def test_a_waived_commit_that_stops_being_a_signal_must_be_deleted(self) -> None:
        waived = next(iter(ordering.PRE_APPROVAL_SIGNALS))
        with self.assertRaisesRegex(ordering.OrderingError, "pre-approval-waiver"):
            ordering.enforce_waiver_is_live([])
        ordering.enforce_waiver_is_live(
            [(commit, commit, []) for commit in ordering.PRE_APPROVAL_SIGNALS]
        )
        self.assertIn(waived, ordering.PRE_APPROVAL_SIGNALS)

    def test_every_waived_commit_is_a_full_sha(self) -> None:
        for commit in ordering.PRE_APPROVAL_SIGNALS:
            with self.subTest(commit=commit):
                self.assertRegex(commit, r"^[0-9a-f]{40}$")

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

        partials = tuple({key: value} for key, value in {
            "GITHUB_EVENT_NAME": "push",
            "GITHUB_REF": "refs/heads/feature/core-modularization",
            "GITHUB_SHA": head,
            "GITHUB_BASE_REF": "feature/core-modularization",
            "GITHUB_HEAD_REF": "slice/example",
        }.items())
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
            "GITHUB_BASE_REF": "release/1.0",
            "GITHUB_HEAD_REF": "slice/example",
        }
        with mock.patch.dict(os.environ, wrong_base, clear=True), self.assertRaisesRegex(
            ordering.OrderingError, "event-base"
        ):
            ordering.validate_event(head)

    def test_every_gated_base_is_accepted(self) -> None:
        """The accepted set must cover every base the workflow triggers on.

        `main` used to be this test's rejected example. It is a gated base now,
        which is the point: the checker rejected a pull request into `testing`
        with rule=event-base, so widening the workflow trigger alone left the
        gate failing every run.
        """
        head = "a" * 40
        for base in ("main", "testing", "feature/core-modularization",
                     "aimee/feat/1234", "agent/some-topic"):
            with self.subTest(base=base), mock.patch.dict(
                os.environ,
                {
                    "GITHUB_EVENT_NAME": "pull_request",
                    "GITHUB_REF": "refs/pull/123/merge",
                    "GITHUB_SHA": head,
                    "GITHUB_BASE_REF": base,
                    "GITHUB_HEAD_REF": "slice/example",
                },
                clear=True,
            ):
                ordering.validate_event(head)
            with self.subTest(base=base, event="push"), mock.patch.dict(
                os.environ,
                {
                    "GITHUB_EVENT_NAME": "push",
                    "GITHUB_REF": f"refs/heads/{base}",
                    "GITHUB_SHA": head,
                },
                clear=True,
            ):
                ordering.validate_event(head)

    def test_an_ungated_base_is_still_rejected(self) -> None:
        head = "a" * 40
        for base in ("release/1.0", "aimee/feat", "agent", "wip"):
            with self.subTest(base=base), mock.patch.dict(
                os.environ,
                {
                    "GITHUB_EVENT_NAME": "push",
                    "GITHUB_REF": f"refs/heads/{base}",
                    "GITHUB_SHA": head,
                },
                clear=True,
            ), self.assertRaisesRegex(ordering.OrderingError, "event-ref"):
                ordering.validate_event(head)

    def test_gated_bases_match_the_workflow_triggers(self) -> None:
        """A base the workflow fires on but this rejects fails every run."""
        workflow = (REPO_ROOT / ".github/workflows/module-inventory.yml").read_text(
            encoding="utf-8"
        )
        triggers = re.findall(r"^\s*branches: \[(.+)\]$", workflow, re.MULTILINE)
        self.assertTrue(triggers)
        for line in triggers:
            for raw in line.split(","):
                base = raw.strip().strip("'\"")
                if base.endswith("/**"):
                    base = base[: -len("**")] + "example"
                with self.subTest(base=base):
                    self.assertTrue(
                        ordering.is_gated_base(base),
                        f"{base!r} triggers the workflow but validate_event rejects it",
                    )

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

    def test_dirty_contract_or_handoff_fails_closed(self) -> None:
        tmp, repo, _ = self.make_repo()
        try:
            contract = repo / ordering.CONTRACT_PATH
            handoff = repo / ordering.HANDOFF_PATH
            contract.parent.mkdir(parents=True)
            handoff.parent.mkdir(parents=True, exist_ok=True)
            contract.write_text("contract\n", encoding="utf-8")
            handoff.write_text("handoff\n", encoding="utf-8")
            self.commit(repo, "metadata")
            contract.write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(ordering.OrderingError, "live-contract-dirty"):
                ordering.require_clean_metadata(repo)
        finally:
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
