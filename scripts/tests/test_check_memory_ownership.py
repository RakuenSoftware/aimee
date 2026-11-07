from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ownership", ROOT / "scripts/check_memory_ownership.py")
assert spec and spec.loader
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class MemoryOwnershipTest(unittest.TestCase):
    def fixture(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        for name in ("src/modules/memory", "server-go/modules/memory", "tests/baselines/modules"):
            (root / name).mkdir(parents=True)
        (root / "src/modules/memory/module.yaml").write_text('{"sources": []}')
        (root / "evidence.go").write_text("package evidence")
        (root / "host.c").write_text("void call(void) { kb_client_memory_recall(); }\n")
        manifest = {"version": 1, "source_commit": "frozen", "native_files": ["src/modules/memory/old.c"],
                    "native_symbols": ["old_memory_function"]}
        (root / checker.inventory.MANIFEST).write_text(json.dumps(manifest))
        ledger = {"source_commit": "frozen", "dispositions": {"host": {
            "owner": "C host", "contract": "Go request/reply", "reason": "transport only",
            "evidence": ["evidence.go"], "cutover_dependency": "owner attached"}}, "original_files": {
            "src/modules/memory/old.c": {"disposition": "host", "replacement_files": ["evidence.go"]}}, "original_symbols": {
            "old_memory_function": {"disposition": "host"}}, "external_files": {"host.c": {
            "disposition": "host", "symbols": ["kb_client_memory_recall"],
            "sha256": hashlib.sha256((root / "host.c").read_bytes()).hexdigest()}}}
        (root / checker.LEDGER).write_text(json.dumps(ledger))
        (root / checker.RETIREMENT).write_text(json.dumps({"source_commit": checker.RETIREMENT_SOURCE, "files": {}}))
        return root, ledger

    def test_accepts_classified_external_c_host(self):
        root, _ = self.fixture()
        checker.validate(root)

    def test_generated_output_is_optional_but_still_reviewed(self):
        root, ledger = self.fixture()
        ledger["generated_files"] = {"host.c": ledger["external_files"].pop("host.c")}
        ledger["generated_files"]["host.c"]["inputs"] = {
            "evidence.go": hashlib.sha256((root / "evidence.go").read_bytes()).hexdigest()}
        (root / checker.LEDGER).write_text(json.dumps(ledger))
        checker.validate(root)
        original = (root / "host.c").read_text()
        (root / "host.c").unlink()
        checker.validate(root)
        (root / "host.c").write_text(original + "int unexpected;\n")
        with self.assertRaisesRegex(ValueError, "changed without ownership review"):
            checker.validate(root)
        (root / "host.c").write_text(original)
        (root / "evidence.go").write_text("package changed")
        with self.assertRaisesRegex(ValueError, "generated input needs ownership review"):
            checker.validate(root)

    def test_retirement_snapshot_requires_frozen_source_reference(self):
        root, _ = self.fixture()
        for snapshot in ({"files": {}}, {"source_commit": "unreviewed", "files": {}}):
            (root / checker.RETIREMENT).write_text(json.dumps(snapshot))
            with self.assertRaisesRegex(ValueError, "original external retirement source reference changed"):
                checker.validate(root)

    def test_retirement_preserves_findings_and_refuses_restoration(self):
        root, ledger = self.fixture()
        original = ledger["external_files"].pop("host.c")
        (root / checker.RETIREMENT).write_text(json.dumps({"source_commit": checker.RETIREMENT_SOURCE, "files": {"host.c": original}}))
        ledger["retired_external_files"] = {"host.c": {**original, "retirement": {
            "reason": "unlinked owner retired", "evidence": ["evidence.go"]}}}
        content = (root / "host.c").read_text()
        (root / "host.c").unlink()
        def save():
            (root / checker.LEDGER).write_text(json.dumps(ledger))
        save()
        checker.validate(root)
        for restored in (content, "/* empty renamed implementation */"):
            (root / "host.c").write_text(restored)
            with self.assertRaisesRegex(ValueError, "retired external file restored"):
                checker.validate(root)
            (root / "host.c").unlink()
        row = ledger["retired_external_files"]["host.c"]
        for key in ("symbols", "sha256", "disposition"):
            saved = row.pop(key)
            save()
            with self.assertRaisesRegex(ValueError, "retired external finding changed"):
                checker.validate(root)
            row[key] = saved
        ledger["retired_external_files"].clear()
        save()
        with self.assertRaisesRegex(ValueError, "missing retired external finding disposition"):
            checker.validate(root)

    def test_rejects_unreviewed_new_caller(self):
        root, _ = self.fixture()
        (root / "new.c").write_text("void call(void) { kb_client_memory_recall(); }")
        with self.assertRaisesRegex(ValueError, "ownership needs review"):
            checker.validate(root)

    def test_rejects_changed_adapter_with_same_symbols(self):
        root, _ = self.fixture()
        (root / "host.c").write_text("void call(void) { kb_client_memory_recall(); return; }")
        with self.assertRaisesRegex(ValueError, "changed without ownership review"):
            checker.validate(root)

    def test_rejects_missing_symbol_and_evidence(self):
        for change in ("symbol", "evidence", "empty_evidence", "contract", "cutover_dependency"):
            with self.subTest(change=change):
                root, ledger = self.fixture()
                if change == "symbol":
                    ledger["original_symbols"].clear()
                elif change == "evidence":
                    (root / "evidence.go").unlink()
                elif change == "empty_evidence":
                    ledger["dispositions"]["host"]["evidence"] = []
                else:
                    del ledger["dispositions"]["host"][change]
                (root / checker.LEDGER).write_text(json.dumps(ledger))
                with self.assertRaises(ValueError):
                    checker.validate(root)

    def test_rejects_native_memory_even_if_ledger_updated(self):
        root, _ = self.fixture()
        (root / "server-go/modules/memory/native.c").write_text("int engine(void) { return 0; }")
        with self.assertRaisesRegex(ValueError, "unclassified native violation"):
            checker.validate(root)


if __name__ == "__main__":
    unittest.main()
