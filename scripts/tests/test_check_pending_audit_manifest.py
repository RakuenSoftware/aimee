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

    def test_exact_pending_partition_still_fails_closed(self) -> None:
        (self.proposals / "pending/current.md").write_text(
            "# Current\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        (self.proposals / "pending/unreviewed.md").write_text(
            "# Extra\n\n- **State:** PENDING\n", encoding="utf-8"
        )
        self.write_manifest("2026-08-04")
        with self.assertRaisesRegex(ValueError, "pending set mismatch"):
            checker.main()


if __name__ == "__main__":
    unittest.main()
