#!/usr/bin/env python3
"""Tests for exhaustive standalone C-process export generation."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
EXPORTER = REPO_ROOT / "scripts/export_c_repositories.py"
sys.path.insert(0, str(REPO_ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("export_c_repositories", EXPORTER)
assert SPEC and SPEC.loader
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


class CProcessBuildTests(unittest.TestCase):
    def descriptor(self) -> dict[str, object]:
        return {
            "sources": [
                "src/modules/db2/c/db2_init.c",
                "src/modules/db2/c/db_postgres.c",
                "src/modules/db2/module_adapter.c",
            ],
            "c_build": {
                "compile_definitions": [
                    "AIMEE_DB1_DISABLED",
                    "AIMEE_DISABLE_DB2_SQLITE_SHIM",
                ],
                "include_roots": [
                    "src",
                    "src/modules/db2/c",
                    "src/modules/db2/include",
                ],
                "pkg_config": ["libpq"],
                "system_libraries": [
                    "OpenSSL::Crypto",
                    "Threads::Threads",
                    "ZLIB::ZLIB",
                    "m",
                    "zstd",
                ],
            },
        }

    def test_owned_files_include_contract_sources(self) -> None:
        descriptor = self.descriptor()
        descriptor["contracts"] = ["src/modules/db2/eventcontract/operations.json"]
        owned = exporter.module_owned_files("db2", descriptor)
        self.assertIn("src/modules/db2/eventcontract/operations.json", owned)
        descriptor["contracts"] = "src/modules/db2/eventcontract/operations.json"
        with self.assertRaisesRegex(exporter.ExportError, "contracts must be a string array"):
            exporter.module_owned_files("db2", descriptor)

    def test_external_module_pin_tracks_the_canonical_go_dependency(self) -> None:
        descriptor = exporter.load_json(REPO_ROOT / "src/modules/config/module.yaml")
        contract = exporter.process_contracts.validate()["config"]
        pin = exporter.external_module_pin("config", "required", descriptor, contract)
        self.assertIsNotNone(pin)
        assert pin is not None
        dependency = exporter.go_dependency_version(
            "github.com/RakuenSoftware/aimee-module-config"
        )
        self.assertEqual(pin["repository"],
                         "https://github.com/RakuenSoftware/aimee-module-config.git")
        self.assertEqual(pin["ref"], dependency)
        self.assertEqual(pin["version"], dependency)
        self.assertEqual(pin["commit"], dependency.rsplit("-", 1)[-1])

    def test_generated_header_inputs_are_owned_and_cmake_generates_out_of_tree(self) -> None:
        descriptor = self.descriptor()
        descriptor["c_build"]["generated_headers"] = [{
            "entries": [
                {"source": "src/modules/db2/c/schema.sql",
                 "symbol": "AIMEE_DB2_SCHEMA_SQL"},
                {"source": "src/modules/db2/c/schema_sqlite.sql",
                 "symbol": "AIMEE_DB2_SCHEMA_SQLITE_SQL"},
            ],
            "output": "schema_data.h",
        }]
        owned = exporter.module_owned_files("db2", descriptor)
        self.assertIn("src/modules/db2/c/schema.sql", owned)
        self.assertIn("src/modules/db2/c/schema_sqlite.sql", owned)
        cmake = exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)
        self.assertIn("find_package(Python3 REQUIRED COMPONENTS Interpreter)", cmake)
        self.assertIn("scripts/generate_c_embedded_header.py", cmake)
        self.assertIn("${MODULE_GENERATED_DIR}/schema_data.h", cmake)
        self.assertIn("--entry AIMEE_DB2_SCHEMA_SQL", cmake)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/src/modules/db2/c/schema.sql", cmake)
        self.assertIn("${MODULE_GENERATED_DIR}", cmake)

    def test_header_dependencies_are_materialized_but_not_compiled(self) -> None:
        descriptor = self.descriptor()
        descriptor["c_build"]["header_dependencies"] = [
            "src/headers/aimee.h",
            "src/headers/config_embedder_dims.h",
        ]
        files = exporter.module_repository_files("db2", descriptor)
        self.assertIn("src/headers/aimee.h", files)
        self.assertIn("src/headers/config_embedder_dims.h", files)
        self.assertNotIn(
            "src/headers/aimee.h", exporter.module_owned_files("db2", descriptor)
        )
        cmake = exporter.c_process_cmake(
            "db2", "aimee-module-db2", "1.2.3", descriptor
        )
        self.assertNotIn("src/headers/aimee.h", cmake)

        for dependencies, message in (
            (["../outside.h"], "unsafe header dependency"),
            (["src/modules/db2/c/db2.h"], "must be owned"),
            (["src/headers/z.h", "src/headers/a.h"], "sorted and unique"),
        ):
            mutated = self.descriptor()
            mutated["c_build"]["header_dependencies"] = dependencies
            with self.subTest(message=message), self.assertRaisesRegex(
                exporter.ExportError, message
            ):
                exporter.c_process_cmake(
                    "db2", "aimee-module-db2", "1.2.3", mutated
                )

    @unittest.skipUnless(shutil.which("cmake"), "cmake is not installed")
    def test_generated_header_cmake_builds_from_a_clean_source_tree(self) -> None:
        descriptor = self.descriptor()
        descriptor["sources"] = ["src/modules/db2/store.c"]
        descriptor["c_build"] = {
            "generated_headers": [{
                "entries": [{
                    "source": "src/modules/db2/schema.sql",
                    "symbol": "AIMEE_DB2_SCHEMA_SQL",
                }],
                "output": "schema_data.h",
            }],
            "include_roots": ["src/modules/db2"],
            "pkg_config": [],
            "system_libraries": [],
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            module = root / "module"
            prefix = root / "prefix/lib/cmake/aimee-core"
            (module / "runtime").mkdir(parents=True)
            (module / "scripts").mkdir()
            (module / "grants").mkdir()
            (module / "src/modules/db2").mkdir(parents=True)
            prefix.mkdir(parents=True)
            (module / "runtime/main.c").write_text(
                "int embedded_value(void); int main(void) { return embedded_value(); }\n",
                encoding="utf-8",
            )
            (module / "grants/module.grant.in").write_text(
                "version=1\n", encoding="utf-8"
            )
            (module / "src/modules/db2/store.c").write_text(
                '#include "schema_data.h"\n'
                "int embedded_value(void) { return AIMEE_DB2_SCHEMA_SQL[0] == 's' ? 0 : 1; }\n",
                encoding="utf-8",
            )
            (module / "src/modules/db2/schema.sql").write_text(
                "select 1;\n", encoding="utf-8"
            )
            shutil.copy2(
                REPO_ROOT / "scripts/generate_c_embedded_header.py",
                module / "scripts/generate_c_embedded_header.py",
            )
            (module / "CMakeLists.txt").write_text(
                exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor),
                encoding="utf-8",
            )
            (prefix / "aimee-coreConfig.cmake").write_text(
                "add_library(aimee-core-event-bus-client INTERFACE IMPORTED)\n"
                "add_library(aimee::aimee-core-event-bus-client ALIAS "
                "aimee-core-event-bus-client)\n",
                encoding="utf-8",
            )
            (prefix / "aimee-coreConfigVersion.cmake").write_text(
                'set(PACKAGE_VERSION "1.2.3")\n'
                "set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
                "set(PACKAGE_VERSION_EXACT TRUE)\n",
                encoding="utf-8",
            )
            build = root / "build"
            configured = subprocess.run(
                ["cmake", "-S", str(module), "-B", str(build),
                 f"-DCMAKE_PREFIX_PATH={root / 'prefix'}"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
            self.assertEqual(configured.returncode, 0, configured.stderr)
            compiled = subprocess.run(
                ["cmake", "--build", str(build), "--parallel", "2"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            self.assertTrue((build / "generated/schema_data.h").is_file())
            self.assertFalse((module / "schema_data.h").exists())
            ran = subprocess.run([str(build / "aimee-module-db2")], check=False)
            self.assertEqual(ran.returncode, 0)

    def test_cmake_compiles_every_owned_source_once(self) -> None:
        cmake = exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", self.descriptor())
        for source in self.descriptor()["sources"]:
            self.assertEqual(cmake.count(source), 1)
        self.assertIn("find_package(aimee-core 1.2.3 EXACT CONFIG REQUIRED)", cmake)
        self.assertIn("find_package(OpenSSL REQUIRED)", cmake)
        self.assertIn("find_package(Threads REQUIRED)", cmake)
        self.assertIn("find_package(ZLIB REQUIRED)", cmake)
        self.assertIn("pkg_check_modules(MODULE_PKG REQUIRED IMPORTED_TARGET libpq)", cmake)
        self.assertIn("PkgConfig::MODULE_PKG", cmake)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/src/modules/db2/c", cmake)
        self.assertIn("target_compile_definitions(aimee-module-db2 PRIVATE", cmake)
        self.assertIn("AIMEE_DB1_DISABLED", cmake)
        self.assertNotIn("runtime/main.c src/modules/db2/module_adapter.c", cmake)

    def test_descriptor_order_is_required_for_reproducible_cmake(self) -> None:
        for field in ("sources", "compile_definitions", "include_roots", "pkg_config",
                      "system_libraries"):
            descriptor = self.descriptor()
            target = descriptor["sources"] if field == "sources" else descriptor["c_build"][field]
            if len(target) == 1:
                target.append("libssl")
            target.reverse()
            with self.subTest(field=field), self.assertRaisesRegex(
                exporter.ExportError, "sorted and unique"
            ):
                exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)

    def test_missing_build_contract_or_sources_fails_closed(self) -> None:
        descriptor = self.descriptor()
        descriptor.pop("c_build")
        with self.assertRaisesRegex(exporter.ExportError, "exact c_build fields"):
            exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)
        descriptor = self.descriptor()
        descriptor["sources"] = []
        with self.assertRaisesRegex(exporter.ExportError, "descriptor-owned sources"):
            exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)

    def test_non_c_source_and_cmake_injection_are_rejected(self) -> None:
        descriptor = self.descriptor()
        descriptor["sources"][0] = "src/modules/db2/c/provider.cpp"
        with self.assertRaisesRegex(exporter.ExportError, "must all be .c"):
            exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)

        for field, value in (
            ("compile_definitions", "BAD=1"),
            ("include_roots", "../outside"),
            ("pkg_config", "libpq)\nmessage(FATAL_ERROR injected)"),
            ("system_libraries", "m;injected"),
        ):
            descriptor = self.descriptor()
            descriptor["c_build"][field].append(value)
            descriptor["c_build"][field].sort()
            with self.subTest(field=field), self.assertRaisesRegex(exporter.ExportError, "unsafe"):
                exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)

        descriptor = self.descriptor()
        descriptor["c_build"]["system_libraries"].append("Unknown::Target")
        descriptor["c_build"]["system_libraries"].sort()
        with self.assertRaisesRegex(exporter.ExportError, "unsupported imported CMake target"):
            exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)

        for generated, message in (
            ([{"output": "../schema.h", "entries": [
                {"source": "src/schema.sql", "symbol": "SCHEMA_SQL"},
            ]}], "outputs"),
            ([{"output": "schema.h", "entries": [
                {"source": "../schema.sql", "symbol": "SCHEMA_SQL"},
            ]}], "unsafe generated input"),
            ([{"output": "schema.h", "entries": [
                {"source": "src/schema.sql", "symbol": "BAD=1"},
            ]}], "unsafe generated symbol"),
        ):
            descriptor = self.descriptor()
            descriptor["c_build"]["generated_headers"] = generated
            with self.subTest(message=message), self.assertRaisesRegex(
                exporter.ExportError, message
            ):
                exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)

    def test_repeated_imported_targets_discover_each_package_once(self) -> None:
        descriptor = self.descriptor()
        descriptor["c_build"]["system_libraries"].append("OpenSSL::SSL")
        descriptor["c_build"]["system_libraries"].sort()
        cmake = exporter.c_process_cmake("db2", "aimee-module-db2", "1.2.3", descriptor)
        self.assertEqual(cmake.count("find_package(OpenSSL REQUIRED)"), 1)
        self.assertIn("OpenSSL::Crypto", cmake)
        self.assertIn("OpenSSL::SSL", cmake)

    def test_runtime_bundle_emits_an_exhaustive_non_amalgamated_c_build(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "repo"
            bundle = Path(temporary) / "bundle"
            descriptor_path = root / "src/modules/db2/module.yaml"
            descriptor_path.parent.mkdir(parents=True)
            descriptor = {
                "id": "db2",
                **self.descriptor(),
            }
            descriptor_path.write_text(json.dumps(descriptor), encoding="utf-8")
            inventory = root / "inventory.json"
            inventory.write_text(
                json.dumps({"required": ["db2"], "optional": []}), encoding="utf-8"
            )
            contracts_path = root / "process-contracts.json"
            contracts_path.write_text('{"clients": []}\n', encoding="utf-8")
            contract = {
                "execution": "process",
                "runtime": "c",
                "principal_ref": 29,
                "placements": ["kb"],
                "stages": [{"id": 1, "name": "lifecycle", "event_kind": 11521}],
            }
            with mock.patch.object(exporter, "ROOT", root), \
                    mock.patch.object(exporter, "INVENTORY", inventory), \
                    mock.patch.object(exporter.process_contracts, "CONTRACTS", contracts_path), \
                    mock.patch.object(exporter.process_contracts, "validate",
                                      return_value={"db2": contract}):
                self.assertEqual(exporter.export_runtime_bundle(bundle), 1)

            build = json.loads((bundle / "c-build.json").read_text(encoding="utf-8"))
            self.assertEqual(build["modules"], [{
                "id": "db2",
                "binary": "aimee-module-db2",
                "main": "src/aimee-module-db2.c",
                **self.descriptor()["c_build"],
                "sources": self.descriptor()["sources"],
            }])
            main = (bundle / "src/aimee-module-db2.c").read_text(encoding="utf-8")
            self.assertIn("extern aimee_module_status_t aimee_module_handler", main)
            self.assertNotIn("db2_init", main)
            self.assertIn("db2\t/usr/local/libexec/aimee-modules/aimee-module-db2",
                          (bundle / "kb.modules").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
