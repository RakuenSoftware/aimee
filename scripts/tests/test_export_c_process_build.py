#!/usr/bin/env python3
"""Tests for exhaustive standalone C-process export generation."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
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
        self.assertNotIn("runtime/main.c src/modules/db2/module_adapter.c", cmake)

    def test_descriptor_order_is_required_for_reproducible_cmake(self) -> None:
        for field in ("sources", "include_roots", "pkg_config", "system_libraries"):
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
