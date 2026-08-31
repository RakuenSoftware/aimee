#!/usr/bin/env python3
"""Failure-mode tests for the module documentation catalog."""

from __future__ import annotations

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/check_module_docs.py"
SPEC = importlib.util.spec_from_file_location("check_module_docs", SCRIPT)
assert SPEC and SPEC.loader
docs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(docs)


def document(dep: str = "beta", toggle: str = "supported") -> str:
    bodies = {
        name: (
            f"Verified `{name}` contract content names its current source owner, observable behavior, "
            "consumer boundary, and known limitation for this focused module fixture."
        )
        for name in docs.SECTIONS
    }
    bodies["Dependencies and consumers"] = (
        f"- `{dep}`: required test dependency with a verified source consumer, an explicit ownership "
        "boundary, and a documented failure expectation for this fixture."
    )
    bodies["Configuration and activation"] = (
        f"- `runtime_toggle.{toggle}`: `false`; compile-time lifecycle with no runtime enable control, "
        "no hidden startup switch, and a verified always-present reference path."
    )
    return "# alpha module\n\n" + "\n\n".join(
        f"## {name}\n\n{bodies[name]}" for name in docs.SECTIONS
    ) + "\n"


class ModuleDocTests(unittest.TestCase):
    def fixture(self, root: Path) -> tuple[Path, Path, Path]:
        modules = root / "src/modules"
        catalog = root / "docs/modules"
        modules.mkdir(parents=True)
        catalog.mkdir(parents=True)
        for module_id, deps in (("alpha", ["beta"]), ("beta", [])):
            directory = modules / module_id
            directory.mkdir()
            (directory / "module.yaml").write_text(
                json.dumps({"descriptor_version": 1, "id": module_id, "dependencies": deps,
                            "runtime_toggle": {"supported": False}}), encoding="utf-8"
            )
        (catalog / "alpha.md").write_text(document(), encoding="utf-8")
        status = root / "status.json"
        status.write_text(json.dumps({"schema_version": 1, "substantive": ["alpha"],
                                      "debt": ["beta"]}), encoding="utf-8")
        return modules, catalog, status

    def test_valid_report_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules, catalog, status = self.fixture(root)
            first = docs.run(root, modules, catalog, status)
            self.assertEqual(first, docs.run(root, modules, catalog, status))
            self.assertEqual(first, "PASS alpha\nDEBT beta\nSUMMARY debt=beta\n")

    def test_orphan_doc_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules, catalog, status = self.fixture(root)
            (catalog / "orphan.md").write_text("orphan", encoding="utf-8")
            with self.assertRaisesRegex(docs.DocError, "orphan"):
                docs.run(root, modules, catalog, status)

    def test_catalog_readme_is_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules, catalog, status = self.fixture(root)
            (catalog / "README.md").write_text("# Module catalog\n", encoding="utf-8")
            self.assertEqual(
                docs.run(root, modules, catalog, status),
                "PASS alpha\nDEBT beta\nSUMMARY debt=beta\n",
            )

    def test_missing_and_out_of_order_sections_fail(self) -> None:
        for mutation in (lambda text: text.replace("## Compatibility", "### Compatibility"),
                         lambda text: text.replace("## Public contracts", "## Zed contracts")):
            with self.subTest(), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                modules, catalog, status = self.fixture(root)
                path = catalog / "alpha.md"
                path.write_text(mutation(path.read_text(encoding="utf-8")), encoding="utf-8")
                with self.assertRaisesRegex(docs.DocError, "sections differ"):
                    docs.run(root, modules, catalog, status)

    def test_dependency_and_toggle_mismatches_fail(self) -> None:
        for replacement, message in (("`beta`", "`alpha`"),
                                     ("runtime_toggle.supported", "runtime_toggle.unknown")):
            with self.subTest(), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                modules, catalog, status = self.fixture(root)
                path = catalog / "alpha.md"
                path.write_text(path.read_text().replace(replacement, message), encoding="utf-8")
                with self.assertRaisesRegex(docs.DocError, "bullets differ"):
                    docs.run(root, modules, catalog, status)

    def test_toggle_values_and_duplicate_bullets_fail(self) -> None:
        mutations = (
            ("`false`", "`true`", "runtime-toggle"),
            ("- `beta`:", "- `beta`:\n- `beta`:", "duplicate dependency"),
            (
                "- `runtime_toggle.supported`:",
                "- `runtime_toggle.supported`: `false`; duplicate\n- `runtime_toggle.supported`:",
                "duplicate runtime-toggle",
            ),
            (
                "- `runtime_toggle.supported`:",
                "- `runtime_toggle.supported`: enabled; contradiction\n- `runtime_toggle.supported`:",
                "duplicate runtime-toggle",
            ),
            (
                "`false`; compile-time",
                "`false`; compile-time\n- `runtime_toggle.supported`:",
                "duplicate runtime-toggle",
            ),
            (
                "`false`; compile-time",
                "`false`; compile-time\n- `runtime_toggle.supported`:enabled",
                "duplicate runtime-toggle",
            ),
        )
        for old, new, error in mutations:
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                modules, catalog, status = self.fixture(root)
                path = catalog / "alpha.md"
                path.write_text(path.read_text().replace(old, new), encoding="utf-8")
                with self.assertRaisesRegex(docs.DocError, error):
                    docs.run(root, modules, catalog, status)

    def test_malformed_toggle_value_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules, catalog, status = self.fixture(root)
            descriptor = modules / "alpha/module.yaml"
            data = json.loads(descriptor.read_text())
            data["runtime_toggle"]["supported"] = "maybe"
            descriptor.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(docs.DocError, "map string keys to booleans"):
                docs.run(root, modules, catalog, status)

    def test_malformed_descriptor_and_status_partition_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules, catalog, status = self.fixture(root)
            (modules / "alpha/module.yaml").write_text(
                json.dumps({"id": "alpha", "dependencies": "beta",
                            "runtime_toggle": {"supported": False}}), encoding="utf-8"
            )
            with self.assertRaisesRegex(docs.DocError, "dependencies"):
                docs.run(root, modules, catalog, status)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules, catalog, status = self.fixture(root)
            status.write_text(json.dumps({"schema_version": 1, "substantive": ["alpha"],
                                          "debt": []}), encoding="utf-8")
            with self.assertRaisesRegex(docs.DocError, "partition"):
                docs.run(root, modules, catalog, status)

    def test_debt_doc_requires_promotion(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules, catalog, status = self.fixture(root)
            (catalog / "beta.md").write_text(document(dep="alpha"), encoding="utf-8")
            with self.assertRaisesRegex(docs.DocError, "remains in debt"):
                docs.run(root, modules, catalog, status)

    def test_h1_and_realistic_placeholder_fail(self) -> None:
        for old, new, error in (
            ("# alpha module", "# audit module", "first non-blank"),
            (
                "Verified `Purpose and non-goals` contract content names its current source owner, observable behavior, consumer boundary, and known limitation for this focused module fixture.",
                "Documentation pending review for `alpha`; see module owner for details and updates.",
                "lacks substantive",
            ),
        ):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                modules, catalog, status = self.fixture(root)
                path = catalog / "alpha.md"
                path.write_text(path.read_text().replace(old, new), encoding="utf-8")
                with self.assertRaisesRegex(docs.DocError, error):
                    docs.run(root, modules, catalog, status)

    @unittest.skipUnless(hasattr(os, "symlink"), "symlinks unavailable")
    def test_symlinked_module_docs_and_status_fail(self) -> None:
        for target_kind in ("module", "docs", "status"):
            with self.subTest(target_kind=target_kind), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                modules, catalog, status = self.fixture(root)
                if target_kind == "module":
                    original = modules / "alpha"
                    real = root / "alpha-real"
                    original.rename(real)
                    original.symlink_to(real, target_is_directory=True)
                elif target_kind == "docs":
                    real = root / "docs-real"
                    catalog.rename(real)
                    catalog.symlink_to(real, target_is_directory=True)
                else:
                    real = root / "status-real.json"
                    status.rename(real)
                    status.symlink_to(real)
                with self.assertRaisesRegex(docs.DocError, "symlink"):
                    docs.run(root, modules, catalog, status)

    def test_configured_path_escape_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            outer = Path(directory)
            root = outer / "repo"
            root.mkdir()
            modules, catalog, status = self.fixture(root)
            with self.assertRaisesRegex(docs.DocError, "escapes config root"):
                docs.run(root, outer, catalog, status)

    @unittest.skipUnless(hasattr(os, "symlink"), "symlinks unavailable")
    def test_symlinked_config_root_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            outer = Path(directory)
            real_root = outer / "real"
            real_root.mkdir()
            modules, catalog, status = self.fixture(real_root)
            linked_root = outer / "linked"
            linked_root.symlink_to(real_root, target_is_directory=True)
            with self.assertRaisesRegex(docs.DocError, "config root contains symlink"):
                docs.run(
                    linked_root,
                    linked_root / modules.relative_to(real_root),
                    linked_root / catalog.relative_to(real_root),
                    linked_root / status.relative_to(real_root),
                )


if __name__ == "__main__":
    unittest.main()
