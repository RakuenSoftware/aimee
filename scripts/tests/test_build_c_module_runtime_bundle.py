#!/usr/bin/env python3
"""Tests for descriptor-owned C process builds in runtime bundles."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILDER = REPO_ROOT / "scripts/build_c_module_runtime_bundle.py"
SPEC = importlib.util.spec_from_file_location("build_c_module_runtime_bundle", BUILDER)
assert SPEC and SPEC.loader
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class RuntimeBundleBuildTests(unittest.TestCase):
    def fixture(self) -> tuple[tempfile.TemporaryDirectory[str], Path, Path, Path]:
        temporary = tempfile.TemporaryDirectory()
        base = Path(temporary.name)
        root = base / "repo"
        bundle = base / "bundle"
        output = base / "bin"
        files = [
            *builder.CORE_EVENT_BUS_SOURCES,
            "src/modules/db2/c/store.c",
            "src/modules/db2/module_adapter.c",
        ]
        for relative in files:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int aimee_fixture;\n", encoding="utf-8")
        (root / "src/core/event_bus/include").mkdir(parents=True)
        (root / "src/modules/db2/include").mkdir(parents=True)
        main = bundle / "src/aimee-module-db2.c"
        main.parent.mkdir(parents=True)
        main.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        self.write_manifest(bundle)
        return temporary, root, bundle, output

    def write_manifest(self, bundle: Path) -> None:
        value = {
            "schema_version": 1,
            "modules": [{
                "id": "db2",
                "binary": "aimee-module-db2",
                "main": "src/aimee-module-db2.c",
                "sources": [
                    "src/modules/db2/c/store.c",
                    "src/modules/db2/module_adapter.c",
                ],
                "include_roots": ["src/modules/db2/include"],
                "pkg_config": ["libpq"],
                "system_libraries": [
                    "OpenSSL::Crypto",
                    "Threads::Threads",
                    "ZLIB::ZLIB",
                    "m",
                    "zstd",
                ],
            }],
        }
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / builder.BUILD_MANIFEST).write_text(
            json.dumps(value, indent=2) + "\n", encoding="utf-8"
        )

    def executable(self, path: Path, content: str) -> None:
        path.write_text(content, encoding="utf-8")
        path.chmod(path.stat().st_mode | 0o111)

    def test_build_compiles_generated_main_all_sources_and_dependencies(self) -> None:
        temporary, root, bundle, output = self.fixture()
        try:
            log = Path(temporary.name) / "compiler-args.json"
            compiler = Path(temporary.name) / "fake-cc"
            pkg_config = Path(temporary.name) / "fake-pkg-config"
            self.executable(
                compiler,
                "#!/usr/bin/env python3\n"
                "import json, os, pathlib, sys\n"
                "pathlib.Path(os.environ['FAKE_CC_LOG']).write_text(json.dumps(sys.argv[1:]))\n"
                "pathlib.Path(sys.argv[sys.argv.index('-o') + 1]).write_text('binary')\n",
            )
            self.executable(
                pkg_config,
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "print('-I/pkg/include' if sys.argv[1] == '--cflags' else '-L/pkg/lib -lpq')\n",
            )
            environment = os.environ.copy()
            environment["FAKE_CC_LOG"] = str(log)
            result = subprocess.run(
                [sys.executable, "-I", "-S", str(BUILDER),
                 "--bundle", str(bundle), "--output", str(output),
                 "--root", str(root), "--cc", str(compiler),
                 "--pkg-config", str(pkg_config)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=environment, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((output / "aimee-module-db2").read_text(), "binary")
            arguments = json.loads(log.read_text(encoding="utf-8"))
            expected_sources = [
                bundle / "src/aimee-module-db2.c",
                root / "src/modules/db2/c/store.c",
                root / "src/modules/db2/module_adapter.c",
                *(root / relative for relative in builder.CORE_EVENT_BUS_SOURCES),
            ]
            for source in expected_sources:
                self.assertEqual(arguments.count(str(source)), 1)
            for flag in ("-I/pkg/include", "-L/pkg/lib", "-lpq", "-lcrypto",
                         "-pthread", "-lz", "-lm", "-lzstd"):
                self.assertIn(flag, arguments)
        finally:
            temporary.cleanup()

    def test_manifest_rejects_path_escape_unknown_target_and_unsorted_modules(self) -> None:
        mutations = (
            (lambda value: value["modules"][0].__setitem__("main", "../main.c"),
             "safe relative path"),
            (lambda value: (
                value["modules"][0]["system_libraries"].append("Unknown::Target"),
                value["modules"][0]["system_libraries"].sort(),
            ), "unsupported imported target"),
            (lambda value: value["modules"].append(dict(value["modules"][0])),
             "sorted, and unique"),
        )
        for mutate, message in mutations:
            temporary, _root, bundle, _output = self.fixture()
            try:
                path = bundle / builder.BUILD_MANIFEST
                value = json.loads(path.read_text(encoding="utf-8"))
                mutate(value)
                path.write_text(json.dumps(value), encoding="utf-8")
                with self.subTest(message=message), self.assertRaisesRegex(
                    builder.BuildError, message
                ):
                    builder.load_builds(bundle)
            finally:
                temporary.cleanup()

    def test_missing_owned_source_and_symlink_include_fail_closed(self) -> None:
        temporary, root, bundle, output = self.fixture()
        try:
            (root / "src/modules/db2/c/store.c").unlink()
            module = builder.load_builds(bundle)[0]
            with self.assertRaisesRegex(builder.BuildError, "missing or escapes"):
                builder.compiler_command(module, root, bundle, output, "cc", "pkg-config")
        finally:
            temporary.cleanup()

        temporary, root, bundle, output = self.fixture()
        try:
            include = root / "src/modules/db2/include"
            include.rmdir()
            include.symlink_to(root / "src/modules/db2/c", target_is_directory=True)
            module = builder.load_builds(bundle)[0]
            with self.assertRaisesRegex(builder.BuildError, "not a real directory"):
                builder.compiler_command(module, root, bundle, output, "cc", "pkg-config")
        finally:
            temporary.cleanup()

    def test_placement_builds_only_the_processes_that_placement_runs(self) -> None:
        temporary, root, bundle, output = self.fixture()
        try:
            (bundle / "kb.modules").write_text(
                "db2\t/usr/local/libexec/aimee-modules/aimee-module-db2\n",
                encoding="utf-8")
            # A placement that runs no C process must build none of them, so an
            # image never compiles another placement's build dependencies.
            (bundle / "server.modules").write_text(
                "memory\t/usr/local/libexec/aimee-modules/aimee-module-memory\n",
                encoding="utf-8")
            compiler = Path(temporary.name) / "fake-cc"
            pkg_config = Path(temporary.name) / "fake-pkg-config"
            self.executable(
                compiler,
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "pathlib.Path(sys.argv[sys.argv.index('-o') + 1]).write_text('binary')\n",
            )
            self.executable(pkg_config, "#!/bin/sh\necho\n")
            for placement, expected in (("kb", 1), ("server", 0)):
                for stale in output.glob("*"):
                    stale.unlink()
                code = builder.main([
                    "--bundle", str(bundle), "--output", str(output),
                    "--root", str(root), "--cc", str(compiler),
                    "--pkg-config", str(pkg_config), "--placement", placement,
                ])
                self.assertEqual(code, 0, placement)
                self.assertEqual(len(list(output.glob("*"))), expected, placement)
        finally:
            temporary.cleanup()

    def test_placement_manifest_missing_or_malformed_fails_closed(self) -> None:
        temporary, root, bundle, output = self.fixture()
        try:
            (bundle / "bad.modules").write_text("../escape\tpath\n", encoding="utf-8")
            for placement in ("absent", "bad", "Not-An-Id"):
                code = builder.main([
                    "--bundle", str(bundle), "--output", str(output),
                    "--root", str(root), "--placement", placement,
                ])
                self.assertEqual(code, 1, placement)
        finally:
            temporary.cleanup()

    def test_empty_manifest_is_a_successful_noop(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            bundle = base / "bundle"
            bundle.mkdir()
            (bundle / builder.BUILD_MANIFEST).write_text(
                '{"schema_version": 1, "modules": []}\n', encoding="utf-8"
            )
            output = base / "bin"
            self.assertEqual(builder.build(bundle, output, base, "cc", "pkg-config"), 0)
            self.assertEqual(list(output.iterdir()), [])

    def test_duplicate_json_key_and_bom_fail_closed(self) -> None:
        for raw, message in (
            (b'{"schema_version":1,"schema_version":1,"modules":[]}',
             "duplicate key"),
            (b'\xef\xbb\xbf{"schema_version":1,"modules":[]}', "UTF-8 BOM"),
        ):
            with tempfile.TemporaryDirectory() as temporary:
                bundle = Path(temporary)
                (bundle / builder.BUILD_MANIFEST).write_bytes(raw)
                with self.subTest(message=message), self.assertRaisesRegex(
                    builder.BuildError, message
                ):
                    builder.load_builds(bundle)


if __name__ == "__main__":
    unittest.main()
