#!/usr/bin/env python3
"""Tests for scripts/provide_ouroboros_ring.py: aimee references the ring by contract."""

from __future__ import annotations

import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "provide_ouroboros_ring", REPO_ROOT / "scripts/provide_ouroboros_ring.py")
assert SPEC and SPEC.loader
provide = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provide)


def installed_ring(prefix: Path, version: str, libdir: str = "lib") -> Path:
    pkgconfig = prefix / libdir / "pkgconfig"
    pkgconfig.mkdir(parents=True)
    (pkgconfig / "ouroboros-ring.pc").write_text(
        f"Name: ouroboros-ring\nVersion: {version}\nLibs: -louroboros-ring\n")
    return pkgconfig


class ProvideTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.contract = self.base / "ouroboros-ring.json"
        self.contract.write_text(json.dumps({
            "repository": "https://github.com/RakuenSoftware/ouroboros-ring",
            "compatible": "0.1",
            "release": "v0.1.0",
            "release_tree_sha256": "0" * 64,
        }))
        patches = [
            mock.patch.object(provide, "CONTRACT", self.contract),
            mock.patch.object(provide, "ROOT", self.base),
            mock.patch.dict(os.environ, {"OURO_RING_PREFIX": "", "OURO_RING_SOURCE": ""}),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_compatibility_is_semver_by_contract_range(self) -> None:
        cases = [
            ("0.1.0", "0.1", True), ("v0.1.9", "0.1", True), ("0.2.0", "0.1", False),
            ("0.0.9", "0.1", False), ("1.4.2", "1", True), ("2.0.0", "1", False),
            # A Go pseudo-version names a commit, not a release.
            ("v0.0.0-20261027122832-35b033ec6ce8", "0.1", False), ("0.1", "0.1", False),
            # A floor within the line: the release that added an API aimee uses.
            ("0.1.2", "0.1.2", True), ("v0.1.7", "0.1.2", True), ("0.1.1", "0.1.2", False),
            ("0.2.0", "0.1.2", False), ("1.4.0", "1.4", True), ("1.3.9", "1.4", False),
            ("1.9.0", "1.4", True), ("2.0.0", "1.4", False),
        ]
        for version, compatible, expected in cases:
            self.assertEqual(provide.in_range(version, compatible), expected, (version, compatible))

    def test_contract_range_must_name_a_line(self) -> None:
        data = json.loads(self.contract.read_text())
        for bad in ("0", "0.1.2.3", "^0.1", "x"):
            data["compatible"] = bad
            self.contract.write_text(json.dumps(data))
            with self.assertRaisesRegex(SystemExit, "compatible must be"):
                provide.contract()

    def test_contract_refuses_a_default_release_outside_its_own_range(self) -> None:
        data = json.loads(self.contract.read_text())
        data["release"] = "v0.2.0"
        self.contract.write_text(json.dumps(data))
        with self.assertRaisesRegex(SystemExit, "outside compatible"):
            provide.contract()

    def test_installed_prefix_in_range_is_used_as_is(self) -> None:
        prefix = self.base / "installed"
        pkgconfig = installed_ring(prefix, "0.1.4", libdir="lib64")
        root = self.base / "deps"
        with mock.patch.dict(os.environ, {"OURO_RING_PREFIX": str(prefix)}):
            self.assertEqual(provide.provide(root), pkgconfig)
        self.assertEqual((root / "current").resolve(), prefix.resolve())

    def test_installed_prefix_out_of_range_or_not_a_ring_is_refused(self) -> None:
        prefix = self.base / "too-new"
        installed_ring(prefix, "0.2.0")
        with mock.patch.dict(os.environ, {"OURO_RING_PREFIX": str(prefix)}), \
                self.assertRaisesRegex(SystemExit, "outside compatible 0.1"):
            provide.provide(self.base / "deps")
        empty = self.base / "empty"
        empty.mkdir()
        with mock.patch.dict(os.environ, {"OURO_RING_PREFIX": str(empty)}), \
                self.assertRaisesRegex(SystemExit, "not an installed ring"):
            provide.provide(self.base / "deps")

    def test_release_whose_content_changed_is_refused_before_building(self) -> None:
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w:gz") as tar:
            data = b"0.1.0\n"
            info = tarfile.TarInfo("ouroboros-ring-0.1.0/VERSION")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = archive.getvalue()
        with mock.patch.object(provide.urllib.request, "urlopen", return_value=response), \
                mock.patch.object(provide, "build") as build, \
                self.assertRaisesRegex(SystemExit, "a release tag must never change"):
            provide.provide(self.base / "deps")
        build.assert_not_called()

    def release_archive(self, files: dict[str, bytes]) -> mock.MagicMock:
        tree = self.base / "release-tree"
        for name, data in files.items():
            (tree / name).parent.mkdir(parents=True, exist_ok=True)
            (tree / name).write_bytes(data)
        pin = json.loads(self.contract.read_text())
        pin["release_tree_sha256"] = provide.tree_digest(tree)
        self.contract.write_text(json.dumps(pin))
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w:gz") as tar:
            for name, data in files.items():
                info = tarfile.TarInfo(f"ouroboros-ring-0.1.0/{name}")
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = archive.getvalue()
        return response

    def test_headers_come_from_the_checked_release_without_building(self) -> None:
        response = self.release_archive({
            "VERSION": b"0.1.0\n",
            "c/include/ouroboros/ring.h": b"/* ring */\n",
            "c/src/endpoint.c": b"/* linux only */\n",
        })
        with mock.patch.object(provide.urllib.request, "urlopen", return_value=response), \
                mock.patch.object(provide, "build") as build:
            include = provide.provide_headers(self.base / "deps")
            again = provide.provide_headers(self.base / "deps")
        build.assert_not_called()
        self.assertEqual(include, again)
        self.assertEqual((include / "ouroboros/ring.h").read_bytes(), b"/* ring */\n")
        self.assertEqual(sorted(p.name for p in include.rglob("*") if p.is_file()), ["ring.h"])

    def test_headers_from_a_changed_release_are_refused(self) -> None:
        response = self.release_archive({"c/include/ouroboros/ring.h": b"/* ring */\n"})
        pin = json.loads(self.contract.read_text())
        pin["release_tree_sha256"] = "1" * 64
        self.contract.write_text(json.dumps(pin))
        with mock.patch.object(provide.urllib.request, "urlopen", return_value=response), \
                self.assertRaisesRegex(SystemExit, "a release tag must never change"):
            provide.provide_headers(self.base / "deps")
        self.assertFalse(list((self.base / "deps").glob("*/include")))

    def test_headers_from_an_installation_out_of_range_are_refused(self) -> None:
        prefix = self.base / "installed"
        installed_ring(prefix, "0.1.4")
        with mock.patch.dict(os.environ, {"OURO_RING_PREFIX": str(prefix)}):
            self.assertEqual(provide.provide_headers(self.base / "deps"), prefix.resolve() / "include")
        source = self.base / "checkout"
        source.mkdir()
        (source / "VERSION").write_text("0.2.0\n")
        with mock.patch.dict(os.environ, {"OURO_RING_SOURCE": str(source)}), \
                self.assertRaisesRegex(SystemExit, "outside compatible 0.1"):
            provide.provide_headers(self.base / "deps")

    def test_unsafe_archive_paths_are_refused(self) -> None:
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w:gz") as tar:
            info = tarfile.TarInfo("../escape")
            info.size = 0
            tar.addfile(info, io.BytesIO(b""))
        with self.assertRaisesRegex(SystemExit, "unsafe path"):
            provide.extract(archive.getvalue(), self.base / "out")

    def test_go_module_must_require_a_release_in_range(self) -> None:
        go_mod = self.base / "go.mod"
        with mock.patch.object(provide, "GO_MOD", go_mod):
            go_mod.write_text(f"module x\n\nrequire (\n\t{provide.GO_MODULE} v0.1.3\n)\n")
            provide.check_go()
            go_mod.write_text(
                f"module x\n\nrequire (\n\t{provide.GO_MODULE} v0.0.0-20261027122832-35b033ec6ce8\n)\n")
            with self.assertRaisesRegex(SystemExit, "not a commit"):
                provide.check_go()
            go_mod.write_text("module x\n")
            with self.assertRaisesRegex(SystemExit, "does not require"):
                provide.check_go()

    def test_a_ring_tree_fetched_by_an_older_aimee_is_removed(self) -> None:
        root = self.base / "deps"
        (root / "c/src").mkdir(parents=True)
        (root / "c/src/host.c").write_text("int x;\n")
        (root / "vectors").mkdir()
        (root / "VERSION").write_text("0.1.0\n")
        (root / ".ouroboros-ring-commit").write_text("35b033ec\n")
        kept = root / "v0.1.0-abcdef"
        kept.mkdir()
        (kept / ".provided").write_text("release\n")
        provide.remove_fetched_tree(root)
        self.assertEqual(sorted(entry.name for entry in root.iterdir()), ["v0.1.0-abcdef"])


if __name__ == "__main__":
    unittest.main()
