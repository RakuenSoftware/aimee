from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
import subprocess
import sys
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "reference_builder", ROOT / "scripts/build_reference_memory_fixture.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def archive(name="server-go/go.mod", kind=tarfile.REGTYPE):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w") as output:
        entry = tarfile.TarInfo(name)
        entry.type = kind
        data = b"module frozen-test\n" if kind == tarfile.REGTYPE else b""
        entry.size = len(data)
        output.addfile(entry, io.BytesIO(data))
    return stream.getvalue()


class ReferenceMemoryFixtureTests(unittest.TestCase):
    def test_verify_cli_checks_existing_artifact_without_building(self):
        with tempfile.TemporaryDirectory() as temporary:
            owner = Path(temporary) / 'aimee-module-memory'
            owner.write_bytes(b'unit-verification-artifact-not-functional-owner')
            owner.chmod(0o700)
            receipt = json.loads(builder.MANIFEST.read_text())
            receipt['binary_sha256'] = hashlib.sha256(owner.read_bytes()).hexdigest()
            owner.with_name(owner.name + '.json').write_text(json.dumps(receipt))
            command = [sys.executable, str(ROOT / 'scripts/build_reference_memory_fixture.py'),
                       '--verify', str(owner)]
            # No Go/Git executable can be discovered by this subprocess.
            environment = dict(os.environ, PATH='')
            result = subprocess.run(command, env=environment, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), receipt)
            owner.write_bytes(b'tampered')
            result = subprocess.run(command, env=environment, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('frozen provenance', result.stderr)
            self.assertEqual(owner.read_bytes(), b'tampered')

    def test_fresh_absolute_external_output_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "aimee-module-memory"
            self.assertEqual(builder.output_paths(output)[0], output)
            for invalid in (Path("aimee-module-memory"), root / "aimee-module",
                            ROOT / "aimee-module-memory"):
                with self.assertRaises(ValueError):
                    builder.output_paths(invalid)
            output.write_text("preserve")
            with self.assertRaises(ValueError):
                builder.output_paths(output)
            self.assertEqual(output.read_text(), "preserve")
            output.unlink()
            output.symlink_to(root / "missing")
            with self.assertRaises(ValueError):
                builder.output_paths(output)

    def test_existing_sidecar_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "aimee-module-memory"
            output.with_name(output.name + ".json").write_text("preserve")
            with self.assertRaises(ValueError):
                builder.output_paths(output)

    def test_archive_refuses_traversal_links_and_other_roots(self):
        with tempfile.TemporaryDirectory() as temporary:
            for name, kind in (("server-go/../../escape", tarfile.REGTYPE),
                               ("/server-go/escape", tarfile.REGTYPE),
                               ("other/file", tarfile.REGTYPE),
                               ("server-go/link", tarfile.SYMTYPE)):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    builder.extract_source(archive(name, kind), Path(temporary))

    def test_wrong_tree_or_toolchain_refused_before_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "aimee-module-memory"
            for responses in (["wrong-tree"],
                              [builder.SOURCE_TREE, "go version go1.26.9 linux/amd64"]):
                with mock.patch.object(builder.subprocess, "check_output", side_effect=responses), \
                        mock.patch.object(builder.subprocess, "run") as compile_command:
                    with self.assertRaises(ValueError):
                        builder.build(output)
                    compile_command.assert_not_called()
                    self.assertEqual(list(output.parent.iterdir()), [])

    def test_failed_compilation_publishes_nothing(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "aimee-module-memory"
            with mock.patch.object(builder.subprocess, "check_output", side_effect=[
                    builder.SOURCE_TREE, "go version go1.26.10 linux/amd64", archive()]), \
                    mock.patch.object(builder.subprocess, "run", side_effect=
                                      subprocess.CalledProcessError(1, ["go", "build"])):
                with self.assertRaises(subprocess.CalledProcessError):
                    builder.build(output)
            self.assertEqual(list(output.parent.iterdir()), [])

    def test_frozen_build_command_environment_and_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "aimee-module-memory"
            commands = []

            def check_output(command, **kwargs):
                commands.append(command)
                if command[:2] == ["git", "rev-parse"]:
                    return builder.SOURCE_TREE + "\n"
                if command == ["go", "version"]:
                    return "go version go1.26.10 linux/amd64\n"
                self.assertEqual(command, ["git", "archive", "--format=tar",
                                           builder.SOURCE_COMMIT, "server-go"])
                return archive()

            def compile_fixture(command, **kwargs):
                self.assertEqual(command[:3], ["go", "build", "-o"])
                self.assertEqual(command[4:], ["./cmd/aimee-module"])
                source = kwargs["cwd"]
                self.assertFalse(source.is_relative_to(ROOT))
                self.assertEqual((source / "go.mod").read_text(), "module frozen-test\n")
                for key, value in {"CGO_ENABLED": "0", "GOTOOLCHAIN": builder.fixture_manifest()["go_version"],
                                   "GOWORK": "off", "GOFLAGS": "-mod=readonly"}.items():
                    self.assertEqual(kwargs["env"][key], value)
                binary = Path(command[3])
                binary.write_bytes(b"unit-test-build-output")
                binary.chmod(0o755)

            with mock.patch.object(builder.subprocess, "check_output", check_output), \
                    mock.patch.object(builder.subprocess, "run", compile_fixture):
                receipt = builder.build(output)
            self.assertTrue(output.is_file())
            self.assertFalse(output.is_symlink())
            self.assertTrue(os.access(output, os.X_OK))
            self.assertEqual(receipt["source_commit"], builder.SOURCE_COMMIT)
            self.assertEqual(receipt["purpose"], "external-reference-not-shipped")
            self.assertEqual(receipt["binary_sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(json.loads(output.with_name(output.name + ".json").read_text()), receipt)
            self.assertEqual(sorted(p.name for p in output.parent.iterdir()),
                             [output.name, output.name + ".json"])


if __name__ == "__main__":
    unittest.main()
