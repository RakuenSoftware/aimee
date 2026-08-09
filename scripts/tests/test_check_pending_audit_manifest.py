#!/usr/bin/env python3
"""Tests for selecting and validating the current pending-audit snapshot."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parent.parent / "check_pending_audit_manifest.py"
SPEC = importlib.util.spec_from_file_location("pending_audit_manifest", SCRIPT)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class PendingAuditManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.proposals = self.root / "docs/proposals"
        (self.proposals / "pending").mkdir(parents=True)
        (self.proposals / "done").mkdir()
        self.old_root = checker.ROOT
        self.old_proposals = checker.PROPOSALS
        checker.ROOT = self.root
        checker.PROPOSALS = self.proposals
        self.addCleanup(setattr, checker, "ROOT", self.old_root)
        self.addCleanup(setattr, checker, "PROPOSALS", self.old_proposals)

    def write_manifest(self, date: str, *, review_column: str = "review_record",
                       proposal: str = "current.md") -> None:
        anchor = f"PENDING_AUDIT_{date}.md"
        (self.proposals / anchor).write_text("# evidence\n", encoding="utf-8")
        header = (
            "original\tdisposition\tfinal_path\tresidual_path\tstale_updated\t"
            f"evidence_anchor\t{review_column}\n"
        )
        row = (
            f"{proposal}\tpending_accurate\tdocs/proposals/pending/{proposal}\t-\tno\t"
            f"{anchor}\treview-{date}\n"
        )
        (self.proposals / f"PENDING_AUDIT_{date}.tsv").write_text(
            header + row, encoding="utf-8"
        )

    def test_newest_dated_manifest_is_current_authority(self) -> None:
        (self.proposals / "pending/current.md").write_text(
            "# Current\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        self.write_manifest("2026-07-26", proposal="historical.md")
        self.write_manifest("2026-08-04")
        self.assertEqual(checker.latest_manifest().name, "PENDING_AUDIT_2026-08-04.tsv")
        self.assertEqual(checker.main(), 0)

    def test_row_count_is_derived_and_legacy_review_column_is_accepted(self) -> None:
        (self.proposals / "pending/current.md").write_text(
            "# Current\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        self.write_manifest("2026-08-04", review_column="roundtable")
        self.assertEqual(checker.main(), 0)

    # A manifest is a dated snapshot, so a proposal drafted after it is legitimately
    # absent from it. This previously failed, which made the gate impossible to wire
    # into CI: any new proposal broke it. Superseded deliberately, not by accident.
    def test_proposal_drafted_after_the_snapshot_is_reported_not_fatal(self) -> None:
        (self.proposals / "pending/current.md").write_text(
            "# Current\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        (self.proposals / "pending/drafted-later.md").write_text(
            "# Later\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        self.write_manifest("2026-08-04")
        self.assertEqual(checker.main(), 0)

    # The other direction is still a real defect and must stay fatal: a row claims a
    # proposal is live in pending/ but the file is not there, so the row describes a
    # move nobody reconciled.
    def test_manifest_row_pointing_at_a_vanished_pending_file_fails_closed(self) -> None:
        (self.proposals / "pending/current.md").write_text(
            "# Current\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        self.write_manifest("2026-08-04")
        (self.proposals / "pending/current.md").rename(
            self.proposals / "done/current.md"
        )
        with self.assertRaisesRegex(ValueError, "final path does not exist"):
            checker.main()

    # Same defect, reached the other way: the row's own final_path still resolves,
    # but a DIFFERENT row's residual is gone from pending/.
    def test_missing_residual_from_pending_fails_closed(self) -> None:
        (self.proposals / "pending/current.md").write_text(
            "# Current\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        anchor = "PENDING_AUDIT_2026-08-04.md"
        (self.proposals / anchor).write_text("# evidence\n", encoding="utf-8")
        (self.proposals / "done/archived.md").write_text(
            "# Archived\n\n- **State:** DONE\n\narchived here; residual: residual.md\n",
            encoding="utf-8",
        )
        header = (
            "original\tdisposition\tfinal_path\tresidual_path\tstale_updated\t"
            "evidence_anchor\treview_record\n"
        )
        rows = (
            f"current.md\tpending_accurate\tdocs/proposals/pending/current.md\t-\tno\t{anchor}\tr\n"
            f"archived.md\tpartial_archived\tdocs/proposals/done/archived.md\t"
            f"docs/proposals/pending/residual.md\tarchive_notice\t{anchor}\tr\n"
        )
        (self.proposals / "PENDING_AUDIT_2026-08-04.tsv").write_text(
            header + rows, encoding="utf-8"
        )
        with self.assertRaisesRegex(ValueError, "residual does not exist in pending"):
            checker.main()


if __name__ == "__main__":
    unittest.main()
