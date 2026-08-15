#!/usr/bin/env python3
"""Tests for exhaustive standalone C-process export generation."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


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


if __name__ == "__main__":
    unittest.main()
