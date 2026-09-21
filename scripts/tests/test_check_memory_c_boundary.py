from __future__ import annotations

import json
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO / "scripts/check_memory_c_boundary.py"
SPEC = importlib.util.spec_from_file_location("memory_c_boundary", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)

ALLOWED_C = checker.ALLOWED_C
EXTERNAL_CONNECTION_C = checker.EXTERNAL_CONNECTION_C
KB_CONNECTION_C = checker.KB_CONNECTION_C
BoundaryError = checker.BoundaryError
validate = checker.validate


class MemoryCBoundaryTest(unittest.TestCase):
    def fixture(self) -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / "src/modules/memory").mkdir(parents=True)
        (root / "src/modules/db2/c").mkdir(parents=True)
        for relative in ALLOWED_C:
            path = root / relative
            path.write_text("/* event bus adapter */\n", encoding="utf-8")
        for relative in EXTERNAL_CONNECTION_C:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* event bus adapter */\n", encoding="utf-8")
        for relative in KB_CONNECTION_C:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* provider and event connection adapter */\n", encoding="utf-8")
        (root / "src/modules/memory/module.yaml").write_text(
            json.dumps({"sources": sorted(ALLOWED_C)}), encoding="utf-8"
        )
        return root

    def test_accepts_connection_only_inventory(self) -> None:
        validate(self.fixture())

    def test_rejects_new_memory_c_implementation(self) -> None:
        root = self.fixture()
        (root / "src/modules/memory/ranker.c").write_text("int rank(void);\n", encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_retired_db2_memory_source(self) -> None:
        root = self.fixture()
        (root / "src/modules/db2/c/memory_query.c").write_text("int query(void);\n", encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_retired_platform_memory_policy(self) -> None:
        root = self.fixture()
        path = root / "src/posix/memory.c"
        path.parent.mkdir(parents=True)
        path.write_text("int gate_check_sensitive(void);\n", encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_direct_storage_include(self) -> None:
        root = self.fixture()
        target = root / next(iter(ALLOWED_C))
        target.write_text('#include "db1_client/user_memory.h"\n', encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_external_adapter_storage_include(self) -> None:
        root = self.fixture()
        target = root / next(iter(EXTERNAL_CONNECTION_C))
        target.write_text('#include "db_postgres.h"\n', encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_direct_store_call_without_include(self) -> None:
        root = self.fixture()
        target = root / next(iter(ALLOWED_C))
        target.write_text("void *p = db2_conn();\n", encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_kb_memory_policy_returning_to_c(self) -> None:
        root = self.fixture()
        target = root / next(iter(KB_CONNECTION_C))
        target.write_text("static void mf_build_system_prompt(void) {}\n", encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)


if __name__ == "__main__":
    unittest.main()
