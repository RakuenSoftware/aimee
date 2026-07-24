#!/usr/bin/env python3
"""Tests for the modular-refactor cleanup ledger contract."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "check_cleanup_ledger.py"
SPEC = importlib.util.spec_from_file_location("check_cleanup_ledger", SCRIPT)
assert SPEC and SPEC.loader
ledger = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ledger)


def valid_entry() -> dict[str, object]:
    return {
        "slice": 1,
        "state": "present_and_verified",
        "disposition": "Kept the contract small.",
        "production": {"added": [], "deleted": [], "consolidated": []},
        "fallbacks": [],
        "net_growth": {
            "status": "none",
            "rationale": "No production growth.",
            "rejected_simpler_alternative": "No simpler implementation was needed.",
            "revisit": "Revisit if production code is added.",
        },
        "consumers": ["CI"],
        "blast_radius": "Validation only.",
        "independent_review": "Roundtable approved.",
        "evidence": ["evidence.md"],
    }


class CleanupLedgerTests(unittest.TestCase):
    def write_ledger(self, root: Path, entries: list[dict[str, object]]) -> Path:
        (root / "evidence.md").write_text("evidence\n", encoding="utf-8")
        path = root / "ledger.json"
        path.write_text(json.dumps({"schema_version": 1, "entries": entries}), encoding="utf-8")
        return path

    def test_valid_ledger(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(ledger, "_tracked_files", return_value={"evidence.md"}):
                ledger.validate(self.write_ledger(root, [valid_entry()]), root)

    def test_duplicate_slice_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(ledger, "_tracked_files", return_value={"evidence.md"}):
                with self.assertRaisesRegex(ledger.LedgerError, "duplicate slice"):
                    ledger.validate(self.write_ledger(root, [valid_entry(), valid_entry()]), root)

    def test_unverified_entry_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            entry = valid_entry()
            entry["state"] = "present_and_unverified"
            with mock.patch.object(ledger, "_tracked_files", return_value={"evidence.md"}):
                with self.assertRaisesRegex(ledger.LedgerError, "present_and_unverified"):
                    ledger.validate(self.write_ledger(root, [entry]), root)

    def test_missing_evidence_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            entry = valid_entry()
            entry["evidence"] = ["missing.md"]
            with mock.patch.object(ledger, "_tracked_files", return_value={"missing.md"}):
                with self.assertRaisesRegex(ledger.LedgerError, "does not exist"):
                    ledger.validate(self.write_ledger(root, [entry]), root)

    def test_unknown_field_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            entry = valid_entry()
            entry["surprise"] = True
            with mock.patch.object(ledger, "_tracked_files", return_value={"evidence.md"}):
                with self.assertRaisesRegex(ledger.LedgerError, "unknown"):
                    ledger.validate(self.write_ledger(root, [entry]), root)

    def test_untracked_evidence_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(ledger, "_tracked_files", return_value=set()):
                with self.assertRaisesRegex(ledger.LedgerError, "not Git-tracked"):
                    ledger.validate(self.write_ledger(root, [valid_entry()]), root)

    def test_malformed_enum_types_fail_closed(self) -> None:
        for field in ("state", "growth"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                entry = valid_entry()
                if field == "state":
                    entry["state"] = []
                else:
                    entry["net_growth"]["status"] = []
                with mock.patch.object(ledger, "_tracked_files", return_value={"evidence.md"}):
                    with self.assertRaisesRegex(ledger.LedgerError, "must be a string"):
                        ledger.validate(self.write_ledger(root, [entry]), root)

    def test_duplicate_json_key_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "ledger.json"
            path.write_text('{"schema_version":1,"schema_version":1,"entries":[]}', encoding="utf-8")
            with self.assertRaisesRegex(ledger.LedgerError, "duplicate object key"):
                ledger.validate(path, root)

    def test_absent_entry_may_have_no_consumers_or_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            entry = valid_entry()
            entry["state"] = "absent_with_reason"
            entry["consumers"] = []
            entry["evidence"] = []
            entry["net_growth"] = {
                "status": "none",
                "rationale": "",
                "rejected_simpler_alternative": "",
                "revisit": "",
            }
            with mock.patch.object(ledger, "_tracked_files", return_value=set()):
                ledger.validate(self.write_ledger(root, [entry]), root)


if __name__ == "__main__":
    unittest.main()
