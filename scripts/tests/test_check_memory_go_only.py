from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "memory_go_only", Path(__file__).resolve().parents[1] / "check_memory_go_only.py"
)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class GoOnlyMemoryTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for relative in checker.MEMORY_ROOTS:
            (self.root / relative).mkdir(parents=True)
        self.write("src/modules/memory/module.yaml", '{"sources": [], "public_headers": []}')
        self.write("server-go/modules/memory/client.go", "package memory\n")
        self.manifest = {
            "version": 1,
            "native_symbols": ["memory_get", "memory_node_kind_t"],
            "native_files": ["src/modules/memory/memory_data_bus.c", "src/modules/memory/memory_ontology.h"],
        }

    def write(self, relative: str, text: str):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def rules(self):
        return {failure["rule"] for failure in checker.violations(self.root, self.manifest)}

    def test_accepts_go_owner_and_unrelated_native_host(self):
        self.write("src/core/host.c", "int host_run(void) { return 0; }\n")
        self.assertEqual(self.rules(), set())

    def test_rejects_nested_native_files_in_either_owner(self):
        for directory in checker.MEMORY_ROOTS:
            for suffix in (".c", ".h", ".S", ".syso", ".inc"):
                with self.subTest(directory=directory, suffix=suffix):
                    path = self.write(str(directory / "nested" / ("adapter" + suffix)), "")
                    self.assertIn("memory-native-file", self.rules())
                    path.unlink()

    def test_rejects_descriptor_entries_even_when_file_was_deleted(self):
        for role in ("sources", "public_headers", "private_headers", "tests"):
            with self.subTest(role=role):
                self.write("src/modules/memory/module.yaml", json.dumps({role: ["src/host/moved.c"]}))
                self.assertIn("memory-native-descriptor", self.rules())

    def test_rejects_relocated_calls_types_aliases_and_dynamic_lookup(self):
        for source in (
            "int run(void) { return memory_get(1); }",
            "typedef memory_node_kind_t host_node_t;",
            "#define host_get memory_get",
            'void *symbol = lookup("memory_get");',
            "int stage = AIMEE_MEMORY_STAGE_DATA;",
        ):
            with self.subTest(source=source):
                self.write("src/server/renamed_client.c", source)
                self.assertIn("memory-native-api", self.rules())

    def test_rejects_forwarding_headers_and_bare_native_include(self):
        for included in ("aimee/memory/new_api.h", "modules/memory/new_api.h", "memory_ontology.h"):
            with self.subTest(included=included):
                self.write("src/host/forward.h", f'#include "{included}"\n')
                self.assertIn("memory-native-include", self.rules())

    def test_rejects_cgo_single_and_grouped_imports(self):
        for imports in ('import "C"', 'import (\n "fmt"\n "C"\n)', 'import alias "C"'):
            with self.subTest(imports=imports):
                self.write("server-go/modules/memory/client.go", f"package memory\n{imports}\n")
                self.assertIn("memory-cgo", self.rules())

    def test_rejects_forwarding_symlink(self):
        target = self.write("src/host/implementation.go", "package memory\n")
        (self.root / "server-go/modules/memory/forward.go").symlink_to(target)
        self.assertIn("memory-source-symlink", self.rules())

    def test_rejects_retired_make_and_cmake_registrations(self):
        for name, source in (
            ("CMakeLists.txt", "target_sources(host PRIVATE elsewhere/memory_data_bus.c)"),
            ("src/tests/Rules.mk", "OBJECTS += $(OBJDIR)/renamed/memory_data_bus.o"),
        ):
            with self.subTest(name=name):
                path = self.write(name, source)
                self.assertIn("memory-native-build", self.rules())
                path.unlink()

    def test_comments_do_not_reintroduce_api_but_urls_do_not_hide_calls(self):
        path = self.write("src/host.c", "/* memory_get was retired */\n// memory_node_kind_t\nint host;\n")
        self.assertEqual(self.rules(), set())
        path.write_text('const char *url = "http://example/"; memory_get(1);\n', encoding="utf-8")
        self.assertIn("memory-native-api", self.rules())

    def test_captured_member_is_not_a_global_api_name(self):
        # Run against the real immutable inventory, not a hand-picked list that
        # would omit the accidental nested-enum member capture.
        self.manifest = json.loads((checker.ROOT / checker.MANIFEST).read_text())
        self.assertIn("mode", self.manifest["native_symbols"])
        self.write("src/host.c", 'struct options { int mode; };\n'
                   'int configure(struct options *o, int mode) { o->mode = mode; return mode; }\n'
                   'const char *json = "{\\"mode\\":\\"compact\\"}";\n')
        self.assertEqual(self.rules(), set())

    def test_actual_temporal_api_and_renamed_enum_are_still_rejected(self):
        self.manifest = json.loads((checker.ROOT / checker.MANIFEST).read_text())
        for source in (
            "memory_temporal_constraint_t request;",
            "typedef memory_temporal_constraint_t renamed_constraint;",
            "enum { MEM_DATE_CONSTRAINT_NONE = 0, MEM_DATE_CONSTRAINT_MATCH } mode;",
            "struct renamed { enum { MEM_DATE_CONSTRAINT_BETWEEN } renamed_mode; };",
            "#define renamed_mode MEM_DATE_CONSTRAINT_AFTER",
        ):
            with self.subTest(source=source):
                self.write("src/host.c", source)
                self.assertIn("memory-native-api", self.rules())

    def test_empty_retirement_inventory_cannot_turn_gate_green(self):
        self.manifest["native_symbols"] = []
        with self.assertRaises(ValueError):
            self.rules()


if __name__ == "__main__":
    unittest.main()
