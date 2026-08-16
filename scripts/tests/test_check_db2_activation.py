#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "check_db2_activation.py"
SPEC = importlib.util.spec_from_file_location("check_db2_activation", SCRIPT)
assert SPEC and SPEC.loader
activation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(activation)


class ActivationTests(unittest.TestCase):
    def fixture(self, enabled: bool = True) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        files = {
            activation.DESCRIPTOR: {
                "enabled_by_default": enabled,
                "sources": ["src/modules/db2/c/store.c"],
            },
            activation.SOURCE_BASELINE: {
                "source_files": {"c": ["src/modules/db2/c/store.c"]},
            },
            activation.DECLARATION_LEDGER: {
                "consumer_classes": [
                    {"path": "src/tests/test_store.c",
                     "classification": "private-implementation-test"},
                ],
                "declarations": [{
                    "symbol": "db2_health_probe",
                    "consumers": ["src/tests/test_store.c"],
                    "review": {"disposition": "wire-operation"},
                }],
            },
        }
        for relative, value in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(value), encoding="utf-8")
        adapter = root / activation.ADAPTER
        adapter.parent.mkdir(parents=True, exist_ok=True)
        adapter.write_text("int backend(void);\n", encoding="utf-8")
        return temporary

    def test_production_state_is_safely_disabled(self) -> None:
        activation.check(Path(__file__).resolve().parents[2])

    def test_complete_enabled_fixture_passes(self) -> None:
        temporary = self.fixture()
        try:
            activation.check(Path(temporary.name))
        finally:
            temporary.cleanup()

    def test_disabled_shell_does_not_claim_activation(self) -> None:
        temporary = self.fixture(False)
        try:
            root = Path(temporary.name)
            (root / activation.ADAPTER).write_text("__attribute__((weak))\n", encoding="utf-8")
            activation.check(root)
        finally:
            temporary.cleanup()

    def test_enabled_activation_requires_source_closure(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            descriptor = json.loads((root / activation.DESCRIPTOR).read_text())
            descriptor["sources"] = []
            (root / activation.DESCRIPTOR).write_text(json.dumps(descriptor))
            with self.assertRaisesRegex(activation.ActivationError, "rule=source-closure"):
                activation.check(root)
        finally:
            temporary.cleanup()

    def test_enabled_activation_rejects_weak_backend_and_direct_caller(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            (root / activation.ADAPTER).write_text(
                "extern int backend(void) __attribute__((weak));\n"
            )
            with self.assertRaisesRegex(activation.ActivationError, "rule=weak-backend"):
                activation.check(root)

            (root / activation.ADAPTER).write_text("int backend(void);\n")
            ledger = json.loads((root / activation.DECLARATION_LEDGER).read_text())
            ledger["consumer_classes"].append({
                "path": "src/kb/kb_main.c", "classification": "kb-generated-client",
            })
            ledger["declarations"][0]["consumers"].append("src/kb/kb_main.c")
            (root / activation.DECLARATION_LEDGER).write_text(json.dumps(ledger))
            with self.assertRaisesRegex(activation.ActivationError, "rule=direct-caller"):
                activation.check(root)
        finally:
            temporary.cleanup()

    def test_enabled_activation_checks_every_reviewed_wire_operation(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            ledger = json.loads((root / activation.DECLARATION_LEDGER).read_text())
            ledger["consumer_classes"].append({
                "path": "src/kb/kb_search.c", "classification": "kb-generated-client",
            })
            ledger["declarations"].append({
                "symbol": "db2_search",
                "consumers": ["src/kb/kb_search.c"],
                "review": {"disposition": "wire-operation"},
            })
            (root / activation.DECLARATION_LEDGER).write_text(json.dumps(ledger))
            with self.assertRaisesRegex(
                    activation.ActivationError,
                    "wire operation db2_search still has direct production callers"):
                activation.check(root)
        finally:
            temporary.cleanup()


if __name__ == "__main__":
    unittest.main()
