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
        (root / "server-go/modules/memory").mkdir(parents=True)
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

    def test_rejects_nested_headers_cgo_and_native_descriptor(self) -> None:
        for relative, source in (
            ("server-go/modules/memory/nested/adapter.h", "int native(void);"),
            ("src/modules/memory/include/adapter.h", "int native(void);"),
            ("server-go/modules/memory/adapter.go", 'package memory\nimport "C"\n'),
            ("src/modules/memory/module.yaml", '{"sources":["src/host/adapter.c"]}'),
        ):
            with self.subTest(relative=relative):
                root = self.fixture()
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
                with self.assertRaisesRegex(BoundaryError, "memory-go-only"):
                    validate(root)

    def test_keeps_bus_and_host_transport_in_c(self) -> None:
        root = self.fixture()
        path = root / "src/core/event_bus/bus_host.c"
        path.parent.mkdir(parents=True)
        path.write_text("int bus_host(void) { return 0; }\n")
        path = root / next(iter(EXTERNAL_CONNECTION_C))
        path.write_text("void *host_call(void) { return aimee_module_json_call(5895, 7); }\n")
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

    def test_rejects_retired_gate_or_extraction_file(self) -> None:
        for name in ("memory_fact_gate.c", "memory_extract_patterns.c",
                     "memory_extract_patterns.h", "memory_assemble_util.h",
                     "memory_pii_gate.c", "memory_pii_gate.h"):
            with self.subTest(name=name):
                root = self.fixture()
                (root / "src/modules/memory" / name).write_text("/* retired */\n", encoding="utf-8")
                with self.assertRaises(BoundaryError):
                    validate(root)

    def test_rejects_relocated_native_client_or_declaration(self) -> None:
        for suffix, declaration in (
            ("c", "int memory_extract_patterns(void) { return 0; }"),
            ("h", "void memory_fact_gate_register_checker(void *checker);"),
            ("h", "#define memory_pattern_scan_turn host_scan"),
            ("h", "static inline int assemble_texts_near_duplicate(void) { return 1; }"),
            ("c", "int memory_pii_turn_requests_sensitive(const char *turn) { return 1; }"),
        ):
            with self.subTest(declaration=declaration):
                root = self.fixture()
                target = root / "src/server" / ("moved_memory." + suffix)
                target.parent.mkdir(parents=True)
                target.write_text(declaration, encoding="utf-8")
                with self.assertRaisesRegex(BoundaryError, "retired-memory-native-client"):
                    validate(root)

    def test_allows_historical_comment_without_native_client(self) -> None:
        root = self.fixture()
        target = root / "src/history.c"
        target.write_text("/* memory_extract_patterns was removed. */\nint history;\n", encoding="utf-8")
        validate(root)

    def test_rejects_direct_storage_include(self) -> None:
        root = self.fixture()
        target = root / next(iter(EXTERNAL_CONNECTION_C))
        target.write_text('#include "db1_client/user_memory.h"\n', encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_adapter_storage_include(self) -> None:
        root = self.fixture()
        target = root / next(iter(ALLOWED_C | EXTERNAL_CONNECTION_C))
        target.write_text('#include "db_postgres.h"\n', encoding="utf-8")
        with self.assertRaises(BoundaryError):
            validate(root)

    def test_rejects_retired_scope_bridge_relocation(self) -> None:
        for symbol in ("db2_memory_scope_context_set", "memory_bus_read_context",
                       "memory_bus_set_context_reader", "memory_bus_add_context"):
            with self.subTest(symbol=symbol):
                root = self.fixture()
                (root / "src/another_owner.c").write_text(
                    f"void {symbol}(void);\n", encoding="utf-8")
                with self.assertRaises(BoundaryError):
                    validate(root)

    def test_rejects_direct_store_call_without_include(self) -> None:
        root = self.fixture()
        target = root / next(iter(EXTERNAL_CONNECTION_C))
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
