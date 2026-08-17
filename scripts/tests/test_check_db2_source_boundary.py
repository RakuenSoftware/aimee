#!/usr/bin/env python3
"""Tests for DB2's source boundary and shrink-only dependency manifest."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts/check_db2_source_boundary.py"
SPEC = importlib.util.spec_from_file_location("check_db2_source_boundary", CHECKER)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class BoundaryTests(unittest.TestCase):
    revision = "a" * 40

    def repo(self) -> tempfile.TemporaryDirectory[str]:
        tmp = tempfile.TemporaryDirectory()
        root = Path(tmp.name)
        files = {
            "src/modules/db2/c/store.c": '#include "store.h"\n#include "aimee.h"\n',
            "src/modules/db2/c/store.h": "int db2_store(void);\n",
            "src/modules/db2/c/schema.sql": "CREATE TABLE sample(id integer);\n",
            "src/headers/aimee.h": "int aimee_runtime(void);\n",
            "src/kb/consumer.c": '#include "modules/db2/c/store.h"\n',
            "src/server/consumer.c": '#include "../modules/db2/c/store.h"\n',
            "src/tests/test_store.c": '#include "modules/db2/c/store.h"\n',
            "src/modules/memory/consumer.c": '#include "modules/db2/c/store.h"\n',
        }
        for relative, content in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        contracts = {
            "components": [
                {"id": "memory", "placements": ["server"]},
            ],
        }
        path = root / checker.CONTRACTS
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(contracts), encoding="utf-8")
        self.write_baseline(root)
        return tmp

    def write_baseline(self, root: Path) -> None:
        path = root / checker.BASELINE
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(checker.build_inventory(root, self.revision), indent=2) + "\n",
            encoding="utf-8",
        )

    def test_production_tree_matches_baseline(self) -> None:
        result = checker.check(REPO_ROOT)
        self.assertEqual(result["source_files"], 280)
        self.assertEqual(result["consumer_files"], 303)
        self.assertEqual(result["include_directives"], 991)

    def test_inventory_is_deterministic_sorted_and_classified(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            first = checker.build_inventory(root, self.revision)
            second = checker.build_inventory(root, self.revision)
            self.assertEqual(first, second)
            rows = first["consumers"]
            self.assertEqual([row["path"] for row in rows], sorted(row["path"] for row in rows))
            classes = {row["path"]: row["classification"] for row in rows}
            self.assertEqual(classes["src/kb/consumer.c"], "kb-generated-client")
            self.assertEqual(classes["src/server/consumer.c"], "server-kb-contract")
            self.assertEqual(classes["src/tests/test_store.c"], "private-implementation-test")
            self.assertEqual(classes["src/modules/memory/consumer.c"], "module-kb-contract")
            self.assertEqual(first["outbound_dependencies"], [{
                "source": "src/modules/db2/c/store.c",
                "header": "aimee.h",
                "resolved": "src/headers/aimee.h",
                "classification": "host-api",
                "count": 1,
            }])
        finally:
            tmp.cleanup()

    def test_module_owned_adapter_is_not_an_outside_consumer(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            adapter = root / "src/modules/db2/module_adapter.c"
            adapter.write_text(
                '#include <aimee/db2/module_api.h>\n', encoding="utf-8"
            )
            inventory = checker.build_inventory(root, self.revision)
            self.assertNotIn(
                adapter.relative_to(root).as_posix(),
                {row["path"] for row in inventory["consumers"]},
            )
            checker.check(root)
        finally:
            tmp.cleanup()

    def test_public_generated_client_is_not_a_private_boundary_consumer(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/kb/consumer.c").write_text(
                '#include <aimee/db2/client.h>\n', encoding="utf-8"
            )
            inventory = checker.build_inventory(root, self.revision)
            self.assertNotIn(
                "src/kb/consumer.c",
                {row["path"] for row in inventory["consumers"]},
            )
            checker.check(root)
        finally:
            tmp.cleanup()

    def test_include_removal_is_allowed(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/kb/consumer.c").write_text("/* migrated */\n", encoding="utf-8")
            result = checker.check(root)
            self.assertEqual(result["consumer_files"], 3)
            self.assertEqual(result["include_directives"], 3)
        finally:
            tmp.cleanup()

    def test_new_consumer_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/kb/new.c").write_text(
                '#include "modules/db2/c/store.h"\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(checker.BoundaryError, "rule=include-growth"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_duplicate_include_growth_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/kb/consumer.c").write_text(
                '#include "modules/db2/c/store.h"\n'
                '#include "modules/db2/c/store.h"\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(checker.BoundaryError, "baseline permits 1"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_new_outbound_dependency_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/headers/config.h").write_text(
                "int config_value(void);\n", encoding="utf-8"
            )
            (root / "src/modules/db2/c/store.c").write_text(
                '#include "store.h"\n#include "aimee.h"\n#include "config.h"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.BoundaryError, "rule=dependency-growth"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_duplicate_outbound_dependency_growth_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/modules/db2/c/store.c").write_text(
                '#include "store.h"\n#include "aimee.h"\n#include "aimee.h"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(checker.BoundaryError, "baseline permits 1"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_unresolved_quoted_include_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/modules/db2/c/store.c").write_text(
                '#include "store.h"\n#include "aimee.h"\n#include "missing_authority.h"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                checker.BoundaryError, "unresolved:missing_authority.h"
            ):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_outbound_dependency_classes_are_explicit(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            headers = {
                "src/core/include/aimee/core/portable.h": "int core_api(void);\n",
                "src/modules/audit/include/aimee/audit/public.h": "int audit_api(void);\n",
                "src/modules/memory/private.h": "int memory_private(void);\n",
                "src/vendor/headers/vendor_api.h": "int vendor_api(void);\n",
                "src/kb/private.h": "int kb_private(void);\n",
            }
            for relative, content in headers.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
            (root / "src/modules/db2/c/store.c").write_text(
                '#include "store.h"\n'
                '#include "aimee.h"\n'
                '#include "aimee/core/portable.h"\n'
                '#include "aimee/audit/public.h"\n'
                '#include "modules/memory/private.h"\n'
                '#include "vendor_api.h"\n'
                '#include "kb/private.h"\n'
                '#include "schema_data.h"\n',
                encoding="utf-8",
            )
            rows = checker.build_inventory(root, self.revision)["outbound_dependencies"]
            classes = {row["resolved"]: row["classification"] for row in rows}
            self.assertEqual(classes["src/core/include/aimee/core/portable.h"],
                             "portable-core-api")
            self.assertEqual(classes["src/modules/audit/include/aimee/audit/public.h"],
                             "module-public-api")
            self.assertEqual(classes["src/modules/memory/private.h"], "module-private-api")
            self.assertEqual(classes["src/vendor/headers/vendor_api.h"], "vendored-system-api")
            self.assertEqual(classes["src/kb/private.h"], "kb-authority-leak")
            self.assertEqual(classes["src/schema_data.h"], "generated-schema-input")
        finally:
            tmp.cleanup()

    def test_private_kb_import_is_forbidden_even_when_baselined(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            private = root / "src/kb/private.h"
            private.write_text("int kb_private(void);\n", encoding="utf-8")
            source = root / "src/modules/db2/c/store.c"
            source.write_text(
                source.read_text(encoding="utf-8") + '#include "kb/private.h"\n',
                encoding="utf-8",
            )
            self.write_baseline(root)
            with self.assertRaisesRegex(checker.BoundaryError, "rule=kb-authority-import"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_outbound_baseline_paths_and_classes_fail_closed(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            value = json.loads((root / checker.BASELINE).read_text(encoding="utf-8"))
            for field, replacement, rule in (
                ("source", "../escape.c", "baseline-path"),
                ("resolved", "../../escape.h", "baseline-path"),
                ("classification", "trusted-by-assertion", "baseline-classification"),
            ):
                tampered = json.loads(json.dumps(value))
                tampered["outbound_dependencies"][0][field] = replacement
                with self.subTest(field=field), self.assertRaisesRegex(
                    checker.BoundaryError, f"rule={rule}"
                ):
                    checker._dependency_rows(tampered["outbound_dependencies"])
        finally:
            tmp.cleanup()

    def test_boundary_file_drift_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            (root / "src/modules/db2/c/new.c").write_text("int new_db2_file;\n", encoding="utf-8")
            with self.assertRaisesRegex(checker.BoundaryError, "rule=source-drift"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_legacy_boundary_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            legacy = root / "src/db2/stray.h"
            legacy.parent.mkdir(parents=True)
            legacy.write_text("int stray;\n", encoding="utf-8")
            with self.assertRaisesRegex(checker.BoundaryError, "rule=legacy-boundary"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_consumer_placement_change_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.CONTRACTS
            value = json.loads(path.read_text(encoding="utf-8"))
            value["components"][0]["placements"] = ["server", "kb"]
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(checker.BoundaryError, "rule=consumer-classification"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_tampered_payload_or_summary_is_rejected(self) -> None:
        for field, rule in (("fingerprint", "baseline-fingerprint"), ("summary", "summary-drift")):
            tmp = self.repo()
            try:
                root = Path(tmp.name)
                path = root / checker.BASELINE
                value = json.loads(path.read_text(encoding="utf-8"))
                if field == "fingerprint":
                    value[field] = "0" * 64
                else:
                    value[field]["consumer_files"] += 1
                path.write_text(json.dumps(value), encoding="utf-8")
                with self.subTest(field=field), self.assertRaisesRegex(
                    checker.BoundaryError, f"rule={rule}"
                ):
                    checker.check(root)
            finally:
                tmp.cleanup()

    def test_reviewed_allowlist_may_only_shrink(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            previous = checker.build_inventory(root, self.revision)
            current = json.loads(json.dumps(previous))
            current["consumers"][0]["includes"][0]["count"] = 0
            # A real shrink removes the zero-count row from the manifest.
            current["consumers"][0]["includes"].pop(0)
            current["consumers"].pop(0)
            checker.enforce_shrink_only(previous, current)

            grown = json.loads(json.dumps(previous))
            grown["consumers"][0]["includes"][0]["count"] += 1
            with self.assertRaisesRegex(checker.BoundaryError, "rule=baseline-growth"):
                checker.enforce_shrink_only(previous, grown)

            added = json.loads(json.dumps(previous))
            added["consumers"].append({
                "path": "src/zz_new.c",
                "classification": "host-generated-client",
                "includes": [{"header": "modules/db2/c/store.h", "count": 1}],
            })
            with self.assertRaisesRegex(checker.BoundaryError, "new allowlist entry"):
                checker.enforce_shrink_only(previous, added)

            reclassified = json.loads(json.dumps(previous))
            reclassified["consumers"][0]["classification"] = "host-generated-client"
            with self.assertRaisesRegex(
                checker.BoundaryError, "rule=baseline-classification"
            ):
                checker.enforce_shrink_only(previous, reclassified)

            dependency_removed = json.loads(json.dumps(previous))
            dependency_removed["outbound_dependencies"] = []
            checker.enforce_shrink_only(previous, dependency_removed)

            dependency_grown = json.loads(json.dumps(previous))
            dependency_grown["outbound_dependencies"][0]["count"] += 1
            with self.assertRaisesRegex(checker.BoundaryError, "rule=baseline-growth"):
                checker.enforce_shrink_only(previous, dependency_grown)

            dependency_added = json.loads(json.dumps(previous))
            dependency_added["outbound_dependencies"].append({
                "source": "src/modules/db2/c/store.c",
                "header": "config.h",
                "resolved": "src/headers/config.h",
                "classification": "host-api",
                "count": 1,
            })
            with self.assertRaisesRegex(checker.BoundaryError, "new outbound dependency"):
                checker.enforce_shrink_only(previous, dependency_added)

            private_to_public = json.loads(json.dumps(previous))
            row = private_to_public["outbound_dependencies"][0]
            row["header"] = "kb_mgmt_contract.h"
            row["resolved"] = "src/kb/kb_mgmt_contract.h"
            row["classification"] = "kb-authority-leak"
            promoted = json.loads(json.dumps(private_to_public))
            promoted_row = promoted["outbound_dependencies"][0]
            promoted_row["resolved"] = "src/headers/kb_mgmt_contract.h"
            promoted_row["classification"] = "host-api"
            checker.enforce_shrink_only(private_to_public, promoted)
            promoted_row["count"] += 1
            with self.assertRaisesRegex(checker.BoundaryError, "new outbound dependency"):
                checker.enforce_shrink_only(private_to_public, promoted)

            vendor_cjson = {
                "consumers": [],
                "outbound_dependencies": [{
                    "source": "src/modules/db2/c/store.c",
                    "header": "cJSON.h",
                    "resolved": "src/vendor/headers/cJSON.h",
                    "classification": "vendored-system-api",
                    "count": 2,
                }],
            }
            owned_cjson = json.loads(json.dumps(vendor_cjson))
            owned_row = owned_cjson["outbound_dependencies"][0]
            owned_row["resolved"] = "src/modules/db2/support/cJSON.h"
            owned_row["classification"] = "module-private-api"
            checker.enforce_shrink_only(vendor_cjson, owned_cjson)
            owned_row["count"] += 1
            with self.assertRaisesRegex(checker.BoundaryError, "new outbound dependency"):
                checker.enforce_shrink_only(vendor_cjson, owned_cjson)
            owned_row["count"] -= 1
            owned_row["resolved"] = "src/modules/db2/support/other.h"
            with self.assertRaisesRegex(checker.BoundaryError, "new outbound dependency"):
                checker.enforce_shrink_only(vendor_cjson, owned_cjson)

            legacy_v1 = json.loads(json.dumps(previous))
            legacy_v1.pop("outbound_dependencies")
            checker.enforce_shrink_only(legacy_v1, previous)
            legacy_growth = json.loads(json.dumps(previous))
            legacy_growth["consumers"][0]["includes"][0]["count"] += 1
            with self.assertRaisesRegex(checker.BoundaryError, "rule=baseline-growth"):
                checker.enforce_shrink_only(legacy_v1, legacy_growth)
        finally:
            tmp.cleanup()

    def test_duplicate_json_key_is_rejected(self) -> None:
        tmp = self.repo()
        try:
            root = Path(tmp.name)
            path = root / checker.BASELINE
            path.write_text('{"schema_version":1,"schema_version":1}\n', encoding="utf-8")
            with self.assertRaisesRegex(checker.BoundaryError, "rule=json-duplicate-key"):
                checker.check(root)
        finally:
            tmp.cleanup()

    def test_cli_is_cwd_independent(self) -> None:
        result = subprocess.run(
            [sys.executable, "-I", "-S", str(CHECKER)],
            cwd="/tmp",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("303 consumers", result.stdout)


if __name__ == "__main__":
    unittest.main()
