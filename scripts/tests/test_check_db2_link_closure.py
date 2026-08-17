#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_db2_link_closure", REPO / "scripts/check_db2_link_closure.py"
)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class LinkClosureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.production_support = checker.SUPPORT_UNITS
        checker.SUPPORT_UNITS = []
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "src/modules/db2/c").mkdir(parents=True)
        (self.root / "src/modules/db2/eventcontract").mkdir(parents=True)
        (self.root / "src/modules/db2/c/a.c").write_text(
            "extern int outside(void);\nint a(void) { return outside(); }\n", encoding="utf-8"
        )
        self.contract = self._contract()
        self._write_all()

    def tearDown(self) -> None:
        checker.SUPPORT_UNITS = self.production_support
        self.temp.cleanup()

    def _contract(self) -> dict[str, object]:
        sources = ["src/modules/db2/c/a.c"]
        result: dict[str, object] = {
            "schema_version": checker.SCHEMA_VERSION,
            "module": "db2",
            "source_revision": "a" * 40,
            "source_fingerprint": checker.source_fingerprint(self.root, sources),
            "probe": {
                "compile_driver": "src/Makefile",
                "extra_c_flags": checker.PROBE_FLAGS,
                "link_mode": "relocatable-no-libraries",
                "helper_objects": [],
                "libraries": [],
                "support_compile_flags": checker.SUPPORT_COMPILE_FLAGS,
                "support_include_roots": checker.SUPPORT_INCLUDE_ROOTS,
            },
            "translation_units": sources,
            "descriptor_support_units": [],
            "unresolved": [{
                "symbol": "outside",
                "references": sources,
                "disposition": "portable-core-promotion",
                "evidence": "Fixture-owned unresolved support dependency.",
            }],
            "summary": {
                "translation_units": 1,
                "descriptor_support_units": 0,
                "unresolved_symbols": 1,
                "dispositions": {
                    name: int(name == "portable-core-promotion")
                    for name in sorted(checker.DISPOSITIONS)
                },
            },
        }
        self._fingerprint(result)
        return result

    @staticmethod
    def _fingerprint(value: dict[str, object]) -> None:
        value.pop("fingerprint", None)
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
        value["fingerprint"] = hashlib.sha256(encoded).hexdigest()

    def _write_all(self) -> None:
        descriptor = {"contracts": [checker.CONTRACT.as_posix()], "sources": []}
        (self.root / checker.DESCRIPTOR).write_text(json.dumps(descriptor), encoding="utf-8")
        (self.root / checker.CONTRACT).write_text(json.dumps(self.contract), encoding="utf-8")

    def _rewrite(self) -> None:
        self._fingerprint(self.contract)
        (self.root / checker.CONTRACT).write_text(json.dumps(self.contract), encoding="utf-8")

    def _refresh_summary(self, value: dict[str, object]) -> None:
        rows = value["unresolved"]
        counts = {name: 0 for name in sorted(checker.DISPOSITIONS)}
        for row in rows:  # type: ignore[union-attr]
            counts[row["disposition"]] += 1
        value["summary"] = {
            "translation_units": len(value["translation_units"]),  # type: ignore[arg-type]
            "descriptor_support_units": len(value["descriptor_support_units"]),  # type: ignore[arg-type]
            "unresolved_symbols": len(rows),  # type: ignore[arg-type]
            "dispositions": counts,
        }
        self._fingerprint(value)

    def _support_row(self) -> dict[str, object]:
        support = self.root / "src/modules/db2/support"
        support.mkdir(parents=True, exist_ok=True)
        (support / "fixture.c").write_text(
            '#include "sketch.h"\nint outside(void) { return 7; }\n', encoding="utf-8"
        )
        (support / "sketch.h").write_text(
            "#include <stddef.h>\n#include <stdint.h>\n", encoding="utf-8"
        )
        return {
            "path": "src/modules/db2/support/fixture.c",
            "header": "src/modules/db2/support/sketch.h",
            "source_sha256": hashlib.sha256(
                (support / "fixture.c").read_bytes()
            ).hexdigest(),
            "header_sha256": hashlib.sha256(
                (support / "sketch.h").read_bytes()
            ).hexdigest(),
            "defines": ["outside"],
            "resolves": ["outside"],
            "allowed_includes": ["sketch.h"],
            "allowed_header_includes": ["stddef.h", "stdint.h"],
            "allowed_undefined": ["memset"],
            "base_references": {"outside": ["src/modules/db2/c/a.c"]},
            "provenance": "Fixture definition and DB2 call site are pinned exactly.",
            "evidence": "Deterministic fixture support with no private dependency.",
        }

    def _copy_support_row(self) -> dict[str, object]:
        support = self.root / "src/modules/db2/support"
        vendor = self.root / "src/vendor/headers"
        support.mkdir(parents=True, exist_ok=True)
        vendor.mkdir(parents=True, exist_ok=True)
        source_text = '#include "fixture.h"\nint extra(void) { return 1; }\nint outside(void) { return 7; }\n'
        header_text = "#include <stddef.h>\n"
        (support / "fixture.c").write_text(source_text, encoding="utf-8")
        (support / "fixture.h").write_text(header_text, encoding="utf-8")
        (self.root / "src/vendor/fixture.c").write_text(source_text, encoding="utf-8")
        (vendor / "fixture.h").write_text(header_text, encoding="utf-8")
        return {
            "path": "src/modules/db2/support/fixture.c",
            "header": "src/modules/db2/support/fixture.h",
            "source_sha256": hashlib.sha256(source_text.encode()).hexdigest(),
            "header_sha256": hashlib.sha256(header_text.encode()).hexdigest(),
            "origin_source": "src/vendor/fixture.c",
            "origin_header": "src/vendor/headers/fixture.h",
            "defines": ["extra", "outside"],
            "resolves": ["outside"],
            "allowed_includes": ["fixture.h"],
            "allowed_header_includes": ["stddef.h"],
            "allowed_undefined": [],
            "base_references": {"outside": ["src/modules/db2/c/a.c"]},
            "provenance": "Exact fixture copy with a pinned generated-input origin and call site.",
            "evidence": "Fixture copy proves extra canonical exports need not be unresolved DB2 imports.",
        }

    def _support_comparison(self) -> tuple[dict[str, object], dict[str, object]]:
        previous = copy.deepcopy(self.contract)
        previous["unresolved"].append({  # type: ignore[union-attr]
            "symbol": "z_remaining",
            "references": ["src/modules/db2/c/a.c"],
            "disposition": "system-link",
            "evidence": "A fixture system dependency that remains unresolved.",
        })
        self._refresh_summary(previous)
        current = copy.deepcopy(previous)
        current["descriptor_support_units"] = [self._support_row()]
        current["unresolved"] = [current["unresolved"][1]]  # type: ignore[index]
        self._refresh_summary(current)
        return previous, current

    def test_valid_metadata(self) -> None:
        checker.check(self.root, run_probe=False)

    def test_duplicate_json_key_fails(self) -> None:
        (self.root / checker.DESCRIPTOR).write_text(
            '{"contracts":[],"contracts":[]}', encoding="utf-8"
        )
        with self.assertRaisesRegex(checker.ClosureError, "json-duplicate-key"):
            checker.check(self.root, run_probe=False)

    def test_symlink_source_fails(self) -> None:
        source = self.root / "src/modules/db2/c/a.c"
        target = self.root / "target.c"
        source.rename(target)
        source.symlink_to(target)
        with self.assertRaisesRegex(checker.ClosureError, "source-file"):
            checker.check(self.root, run_probe=False)

    def test_path_escape_fails(self) -> None:
        self.contract["translation_units"] = ["../outside.c"]
        self._rewrite()
        with self.assertRaisesRegex(checker.ClosureError, "source-path"):
            checker.check(self.root, run_probe=False)

    def test_omitted_source_fails(self) -> None:
        (self.root / "src/modules/db2/c/b.c").write_text("int b;\n", encoding="utf-8")
        with self.assertRaisesRegex(checker.ClosureError, "source-closure"):
            checker.check(self.root, run_probe=False)

    def test_undeclared_missing_source_fails(self) -> None:
        self.contract["translation_units"] = [
            "src/modules/db2/c/a.c", "src/modules/db2/c/missing.c"
        ]
        self._rewrite()
        with self.assertRaisesRegex(checker.ClosureError, "source-file"):
            checker.check(self.root, run_probe=False)

    def test_duplicate_source_fails(self) -> None:
        self.contract["translation_units"] = [
            "src/modules/db2/c/a.c", "src/modules/db2/c/a.c"
        ]
        self._rewrite()
        with self.assertRaisesRegex(checker.ClosureError, "contract-order"):
            checker.check(self.root, run_probe=False)

    def test_source_fingerprint_drift_fails(self) -> None:
        (self.root / "src/modules/db2/c/a.c").write_text("int changed;\n", encoding="utf-8")
        with self.assertRaisesRegex(checker.ClosureError, "source-fingerprint"):
            checker.check(self.root, run_probe=False)

    def test_invalid_disposition_fails(self) -> None:
        self.contract["unresolved"][0]["disposition"] = "guess"  # type: ignore[index]
        self._rewrite()
        with self.assertRaisesRegex(checker.ClosureError, "unresolved-disposition"):
            checker.check(self.root, run_probe=False)

    def test_missing_evidence_fails(self) -> None:
        self.contract["unresolved"][0]["evidence"] = "short"  # type: ignore[index]
        self._rewrite()
        with self.assertRaisesRegex(checker.ClosureError, "unresolved-evidence"):
            checker.check(self.root, run_probe=False)

    def test_helper_object_policy_fails(self) -> None:
        self.contract["probe"]["helper_objects"] = ["core.a"]  # type: ignore[index]
        self._rewrite()
        with self.assertRaisesRegex(checker.ClosureError, "probe-policy"):
            checker.check(self.root, run_probe=False)

    def test_library_policy_fails(self) -> None:
        self.contract["probe"]["libraries"] = ["aimee-core"]  # type: ignore[index]
        self._rewrite()
        with self.assertRaisesRegex(checker.ClosureError, "probe-policy"):
            checker.check(self.root, run_probe=False)

    def test_new_unresolved_symbol_fails(self) -> None:
        actual = {"outside": ["src/modules/db2/c/a.c"], "new_symbol": ["src/modules/db2/c/a.c"]}
        with mock.patch.object(checker, "probe", return_value=actual):
            with self.assertRaisesRegex(checker.ClosureError, "new_symbol"):
                checker.check(self.root)

    def test_resolved_symbol_shrinkage_is_reported_for_review(self) -> None:
        with mock.patch.object(checker, "probe", return_value={}):
            with self.assertRaisesRegex(checker.ClosureError, r"removed=\['outside'\]"):
                checker.check(self.root)

    def test_reference_change_fails(self) -> None:
        actual = {"outside": ["src/modules/db2/c/b.c"]}
        with mock.patch.object(checker, "probe", return_value=actual):
            with self.assertRaisesRegex(checker.ClosureError, "references_changed"):
                checker.check(self.root)

    def test_previous_contract_rejects_new_symbol(self) -> None:
        previous = copy.deepcopy(self.contract)
        current = copy.deepcopy(self.contract)
        current["unresolved"].append({  # type: ignore[union-attr]
            "symbol": "z_new",
            "references": ["src/modules/db2/c/a.c"],
            "disposition": "portable-core-promotion",
            "evidence": "A new unresolved fixture dependency.",
        })
        self._refresh_summary(current)
        with self.assertRaisesRegex(checker.ClosureError, "previous-symbol-growth"):
            checker.compare_contracts(self.root, previous, current)

    def test_previous_contract_allows_added_support_system_import(self) -> None:
        previous, current = self._support_comparison()
        previous["unresolved"].append({  # type: ignore[union-attr]
            "symbol": "zz_resolved",
            "references": ["src/modules/db2/c/a.c"],
            "disposition": "portable-core-promotion",
            "evidence": "Fixture debt removed alongside the support admission.",
        })
        self._refresh_summary(previous)
        unit = current["descriptor_support_units"][0]  # type: ignore[index]
        unit["allowed_undefined"] = ["z_runtime"]
        current["unresolved"].append({  # type: ignore[union-attr]
            "symbol": "z_runtime",
            "references": ["src/modules/db2/support/fixture.c"],
            "disposition": "system-link",
            "evidence": "Reviewed system import introduced only by the admitted support object.",
        })
        self._refresh_summary(current)
        checker.compare_contracts(self.root, previous, current)

    def test_support_import_requires_owned_or_system_provenance(self) -> None:
        unit = self._support_row()
        unit["allowed_undefined"] = ["project_helper"]
        with self.assertRaisesRegex(
            checker.ClosureError, "support-undefined-provenance"
        ):
            checker._validate_support_import_provenance([unit], {"outside"})

    def test_support_import_accepts_another_support_definition(self) -> None:
        unit = self._support_row()
        unit["allowed_undefined"] = ["project_helper"]
        checker._validate_support_import_provenance(
            [unit], {"outside", "project_helper"}
        )

    def test_support_import_accepts_explicit_system_link(self) -> None:
        unit = self._support_row()
        unit["allowed_undefined"] = ["memset"]
        checker._validate_support_import_provenance([unit], {"outside"})

    def test_previous_contract_rejects_added_system_import_from_legacy_unit(self) -> None:
        previous, current = self._support_comparison()
        current["unresolved"].append({  # type: ignore[union-attr]
            "symbol": "z_runtime",
            "references": ["src/modules/db2/c/a.c"],
            "disposition": "system-link",
            "evidence": "Unreviewed system import from a legacy translation unit.",
        })
        self._refresh_summary(current)
        with self.assertRaisesRegex(checker.ClosureError, "previous-symbol-growth"):
            checker.compare_contracts(self.root, previous, current)

    def test_previous_contract_rejects_new_translation_unit(self) -> None:
        (self.root / "src/modules/db2/c/b.c").write_text("int b;\n", encoding="utf-8")
        previous = copy.deepcopy(self.contract)
        current = copy.deepcopy(self.contract)
        current["translation_units"] = [
            "src/modules/db2/c/a.c", "src/modules/db2/c/b.c"
        ]
        self._refresh_summary(current)
        with self.assertRaisesRegex(checker.ClosureError, "previous-source-growth"):
            checker.compare_contracts(self.root, previous, current)

    def test_previous_contract_rejects_removed_translation_unit(self) -> None:
        previous = copy.deepcopy(self.contract)
        previous["translation_units"] = [
            "src/modules/db2/c/a.c", "src/modules/db2/c/b.c",
        ]
        self._refresh_summary(previous)
        with self.assertRaisesRegex(checker.ClosureError, "previous-source-removal"):
            checker.compare_contracts(self.root, previous, self.contract)

    def test_previous_contract_allows_reviewed_symbol_removal(self) -> None:
        previous = copy.deepcopy(self.contract)
        previous["unresolved"].append({  # type: ignore[union-attr]
            "symbol": "z_old",
            "references": ["src/modules/db2/c/a.c"],
            "disposition": "portable-core-promotion",
            "evidence": "An old fixture dependency that was resolved.",
        })
        self._refresh_summary(previous)
        checker.compare_contracts(self.root, previous, self.contract)

    def test_previous_contract_rejects_reference_growth(self) -> None:
        (self.root / "src/modules/db2/c/b.c").write_text("int b;\n", encoding="utf-8")
        previous = copy.deepcopy(self.contract)
        current = copy.deepcopy(self.contract)
        for value in (previous, current):
            value["translation_units"] = [
                "src/modules/db2/c/a.c", "src/modules/db2/c/b.c"
            ]
            self._refresh_summary(value)
        current["unresolved"][0]["references"].append(  # type: ignore[index,union-attr]
            "src/modules/db2/c/b.c"
        )
        self._refresh_summary(current)
        with self.assertRaisesRegex(checker.ClosureError, "previous-reference-growth"):
            checker.compare_contracts(self.root, previous, current)

    def test_previous_contract_allows_exact_support_shrink(self) -> None:
        previous, current = self._support_comparison()
        checker.compare_contracts(self.root, previous, current)

    def test_previous_contract_allows_generated_input_copy_shrink(self) -> None:
        previous = copy.deepcopy(self.contract)
        previous["unresolved"][0][  # type: ignore[index]
            "disposition"
        ] = "descriptor-owned-copy/generated-input"
        previous["unresolved"].append({  # type: ignore[union-attr]
            "symbol": "z_remaining",
            "references": ["src/modules/db2/c/a.c"],
            "disposition": "system-link",
            "evidence": "A fixture system dependency that remains unresolved.",
        })
        self._refresh_summary(previous)
        current = copy.deepcopy(previous)
        current["descriptor_support_units"] = [self._copy_support_row()]
        current["unresolved"] = [current["unresolved"][1]]  # type: ignore[index]
        self._refresh_summary(current)
        checker.compare_contracts(self.root, previous, current)

    def test_support_admission_requires_portable_base_disposition(self) -> None:
        previous, current = self._support_comparison()
        previous["unresolved"][0]["disposition"] = "injected-module-contract"  # type: ignore[index]
        self._refresh_summary(previous)
        with self.assertRaisesRegex(checker.ClosureError, "previous-support-admission"):
            checker.compare_contracts(self.root, previous, current)

    def test_support_admission_requires_declared_resolution(self) -> None:
        previous, current = self._support_comparison()
        current["unresolved"].insert(0, previous["unresolved"][0])  # type: ignore[union-attr,index]
        self._refresh_summary(current)
        with self.assertRaisesRegex(checker.ClosureError, "previous-support-resolution"):
            checker.compare_contracts(self.root, previous, current)

    def test_support_admission_rejects_non_system_reference_growth(self) -> None:
        previous, current = self._support_comparison()
        current["unresolved"][0]["references"].append(  # type: ignore[index,union-attr]
            "src/modules/db2/support/fixture.c"
        )
        self._refresh_summary(current)
        with self.assertRaisesRegex(checker.ClosureError, "previous-reference-growth"):
            checker.compare_contracts(self.root, previous, current)

    def test_reviewed_support_policy_cannot_be_relabelled(self) -> None:
        _, previous = self._support_comparison()
        current = copy.deepcopy(previous)
        current["descriptor_support_units"][0]["evidence"] += " Changed."  # type: ignore[index]
        self._refresh_summary(current)
        with self.assertRaisesRegex(checker.ClosureError, "previous-support-change"):
            checker.compare_contracts(self.root, previous, current)

    def test_support_path_escape_fails(self) -> None:
        unit = self._support_row()
        unit["path"] = "../fixture.c"
        with self.assertRaisesRegex(checker.ClosureError, "source-path"):
            checker._validate_support_units(
                self.root, [unit], ["src/modules/db2/c/a.c"], check_files=True
            )

    def test_support_descriptor_omission_fails(self) -> None:
        descriptor = json.loads((REPO / checker.DESCRIPTOR).read_text(encoding="utf-8"))
        descriptor["sources"].remove("src/modules/db2/support/sketch_primitives.c")
        with mock.patch.object(checker, "SUPPORT_UNITS", self.production_support):
            with self.assertRaisesRegex(checker.ClosureError, "support-descriptor"):
                checker.descriptor_support_policy(REPO, descriptor)

    def test_support_header_descriptor_omission_fails(self) -> None:
        descriptor = json.loads((REPO / checker.DESCRIPTOR).read_text(encoding="utf-8"))
        descriptor["private_headers"].remove("src/modules/db2/support/sketch.h")
        with mock.patch.object(checker, "SUPPORT_UNITS", self.production_support):
            with self.assertRaisesRegex(checker.ClosureError, "support-descriptor"):
                checker.descriptor_support_policy(REPO, descriptor)

    def test_cjson_source_descriptor_omission_fails(self) -> None:
        descriptor = json.loads((REPO / checker.DESCRIPTOR).read_text(encoding="utf-8"))
        descriptor["sources"].remove("src/modules/db2/support/cjson.c")
        with mock.patch.object(checker, "SUPPORT_UNITS", self.production_support):
            with self.assertRaisesRegex(checker.ClosureError, "support-descriptor"):
                checker.descriptor_support_policy(REPO, descriptor)

    def test_cjson_header_descriptor_omission_fails(self) -> None:
        descriptor = json.loads((REPO / checker.DESCRIPTOR).read_text(encoding="utf-8"))
        descriptor["private_headers"].remove("src/modules/db2/support/cJSON.h")
        with mock.patch.object(checker, "SUPPORT_UNITS", self.production_support):
            with self.assertRaisesRegex(checker.ClosureError, "support-descriptor"):
                checker.descriptor_support_policy(REPO, descriptor)

    def test_random_source_descriptor_omission_fails(self) -> None:
        descriptor = json.loads((REPO / checker.DESCRIPTOR).read_text(encoding="utf-8"))
        descriptor["sources"].remove("src/modules/db2/support/random_primitives.c")
        with mock.patch.object(checker, "SUPPORT_UNITS", self.production_support):
            with self.assertRaisesRegex(checker.ClosureError, "support-descriptor"):
                checker.descriptor_support_policy(REPO, descriptor)

    def test_random_header_descriptor_omission_fails(self) -> None:
        descriptor = json.loads((REPO / checker.DESCRIPTOR).read_text(encoding="utf-8"))
        descriptor["private_headers"].remove("src/modules/db2/support/db2_random.h")
        with mock.patch.object(checker, "SUPPORT_UNITS", self.production_support):
            with self.assertRaisesRegex(checker.ClosureError, "support-descriptor"):
                checker.descriptor_support_policy(REPO, descriptor)

    def test_support_forbidden_include_fails(self) -> None:
        unit = self._support_row()
        path = self.root / str(unit["path"])
        path.write_text('#include <stdio.h>\n#include "sketch.h"\n', encoding="utf-8")
        unit["source_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-include"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_support_include_policy_must_be_canonical(self) -> None:
        unit = self._support_row()
        unit["allowed_includes"] = ["sketch.h", "sketch.h"]
        with self.assertRaisesRegex(checker.ClosureError, "contract-order"):
            checker._validate_support_units(
                self.root, [unit], ["src/modules/db2/c/a.c"], check_files=True
            )

    def test_reviewed_support_source_hash_is_required(self) -> None:
        unit = self._support_row()
        path = self.root / str(unit["path"])
        path.write_text("int outside(void) { return 8; }\n", encoding="utf-8")
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-source-hash"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_reviewed_support_header_hash_is_required(self) -> None:
        unit = self._support_row()
        path = self.root / str(unit["header"])
        path.write_text("#include <stdint.h>\n", encoding="utf-8")
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-header-hash"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_owned_copy_source_must_match_vendor_origin(self) -> None:
        unit = self._copy_support_row()
        origin = self.root / str(unit["origin_source"])
        origin.write_text("int outside(void) { return 8; }\n", encoding="utf-8")
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-origin-drift"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_owned_copy_header_must_match_vendor_origin(self) -> None:
        unit = self._copy_support_row()
        origin = self.root / str(unit["origin_header"])
        origin.write_text("#include <stdint.h>\n", encoding="utf-8")
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-origin-drift"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_owned_copy_rejects_symlink_origin(self) -> None:
        unit = self._copy_support_row()
        origin = self.root / str(unit["origin_source"])
        target = self.root / "origin.c"
        origin.rename(target)
        origin.symlink_to(target)
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "source-file"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_support_forbidden_header_include_fails(self) -> None:
        unit = self._support_row()
        path = self.root / str(unit["header"])
        path.write_text(
            "#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n",
            encoding="utf-8",
        )
        unit["header_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-header-include"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_undeclared_support_source_fails(self) -> None:
        unit = self._support_row()
        (self.root / "src/modules/db2/support/other.c").write_text(
            "int other(void) { return 1; }\n", encoding="utf-8"
        )
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-source-closure"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_undeclared_support_header_fails(self) -> None:
        unit = self._support_row()
        (self.root / "src/modules/db2/support/other.h").write_text(
            "#pragma once\n", encoding="utf-8"
        )
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": checker.SUPPORT_INCLUDE_ROOTS},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-header-closure"):
                checker.descriptor_support_policy(self.root, descriptor)

    def test_support_build_include_root_is_required(self) -> None:
        unit = self._support_row()
        descriptor = {
            "sources": [unit["path"]],
            "private_headers": [unit["header"]],
            "c_build": {"include_roots": []},
        }
        with mock.patch.object(checker, "SUPPORT_UNITS", [unit]):
            with self.assertRaisesRegex(checker.ClosureError, "support-build"):
                checker.descriptor_support_policy(self.root, descriptor)

    def _probe_support(self, source_text: str, defines: list[str]) -> None:
        support = self.root / "src/modules/db2/support"
        support.mkdir(parents=True, exist_ok=True)
        (support / "fixture.c").write_text(source_text, encoding="utf-8")
        (self.root / "src/Makefile").write_text(
            "$(OBJDIR)/db2/%.o: modules/db2/c/%.c\n"
            "\t@mkdir -p $(dir $@)\n"
            "\t$(CC) -c $(EXTRA_C_FLAGS) -o $@ $<\n",
            encoding="utf-8",
        )
        unit = {
            "path": "src/modules/db2/support/fixture.c",
            "defines": defines,
            "allowed_undefined": ["memset"],
        }
        checker.probe(self.root, checker.discover_sources(self.root), [unit])

    def test_support_extra_export_fails(self) -> None:
        with self.assertRaisesRegex(checker.ClosureError, "support-exports"):
            self._probe_support(
                "int wanted(void) { return 1; } int extra(void) { return 2; }\n",
                ["wanted"],
            )

    def test_support_new_undefined_symbol_fails(self) -> None:
        with self.assertRaisesRegex(checker.ClosureError, "support-undefined"):
            self._probe_support(
                "extern int forbidden(void); int wanted(void) { return forbidden(); }\n",
                ["wanted"],
            )

    def test_support_weak_export_fails(self) -> None:
        with self.assertRaisesRegex(checker.ClosureError, "support-weak"):
            self._probe_support(
                "__attribute__((weak)) int wanted(void) { return 1; }\n", ["wanted"]
            )

    def test_previous_ref_rejects_option_injection(self) -> None:
        with self.assertRaisesRegex(checker.ClosureError, "previous-ref"):
            checker.check_previous(self.root, "--bad", self.contract)

    def test_previous_ref_allows_absent_initial_contract(self) -> None:
        with mock.patch.object(checker, "_run", side_effect=["commit\n", ""]):
            checker.check_previous(self.root, "base", self.contract)

    def test_previous_ref_does_not_hide_contract_read_failure(self) -> None:
        failure = checker.ClosureError("rule=probe-command: git show failed")
        with mock.patch.object(
            checker,
            "_run",
            side_effect=["commit\n", f"{checker.CONTRACT.as_posix()}\n", failure],
        ):
            with self.assertRaisesRegex(checker.ClosureError, "git show failed"):
                checker.check_previous(self.root, "base", self.contract)

    def test_real_no_library_probe_resolves_only_sibling_units(self) -> None:
        (self.root / "src/modules/db2/c/b.c").write_text(
            "int inside(void) { return 7; }\n", encoding="utf-8"
        )
        (self.root / "src/modules/db2/c/a.c").write_text(
            "extern int inside(void); extern int outside(void);\n"
            "int a(void) { return inside() + outside(); }\n", encoding="utf-8"
        )
        (self.root / "src/Makefile").write_text(
            "$(OBJDIR)/db2/%.o: modules/db2/c/%.c\n"
            "\t@mkdir -p $(dir $@)\n"
            "\t$(CC) -c $(EXTRA_C_FLAGS) -o $@ $<\n",
            encoding="utf-8",
        )
        sources = checker.discover_sources(self.root)
        self.assertEqual(
            checker.probe(self.root, sources),
            {"outside": ["src/modules/db2/c/a.c"]},
        )

    def test_probe_fixture_executes_with_sanitizers_and_fortify(self) -> None:
        source = self.root / "src/modules/db2/c/a.c"
        source.write_text(
            "#include <string.h>\n"
            "extern int outside(void);\n"
            "int a(void) { char x[8] = {0}; return outside() + (int)strlen(x); }\n",
            encoding="utf-8",
        )
        harness = self.root / "harness.c"
        harness.write_text(
            "int a(void); int outside(void) { return 7; }\n"
            "int main(void) { return a() == 7 ? 0 : 1; }\n",
            encoding="utf-8",
        )
        output = self.root / "sanitized-fixture"
        subprocess.run([
            "cc", "-O1", "-g", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=3",
            str(source), str(harness), "-o", str(output),
        ], check=True)
        subprocess.run([str(output)], check=True)

    def test_real_repository_contract_and_probe(self) -> None:
        with mock.patch.object(checker, "SUPPORT_UNITS", self.production_support):
            checker.check(REPO, run_probe=True)

    def test_real_repository_reduces_owned_input_and_bounded_contract_debt(self) -> None:
        contract = json.loads((REPO / checker.CONTRACT).read_text(encoding="utf-8"))
        self.assertEqual(contract["summary"]["unresolved_symbols"], 191)
        self.assertEqual(
            contract["summary"]["dispositions"]["descriptor-owned-copy/generated-input"], 0
        )
        self.assertEqual(contract["summary"]["dispositions"]["system-link"], 139)
        self.assertEqual(
            contract["summary"]["dispositions"]["portable-core-promotion"], 0
        )
        self.assertEqual(
            contract["summary"]["dispositions"]["injected-module-contract"], 52
        )
        self.assertFalse(any(
            row["symbol"].startswith("cJSON_") for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] in {"platform_random_bytes", "platform_random_hex"}
            for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] in {
                "rel_types_seed_at", "rel_types_seed_count", "rel_types_seed_lookup",
            }
            for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] == "aimee_log" for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] == "session_id" for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] in {"cochange_is_hex_sha", "cochange_pairs_for_commit"}
            for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] in {
                "kb_models_endpoint_valid", "kb_models_name_clean", "kb_models_wire_valid",
            }
            for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] == "kb_cert_serial_normalize" for row in contract["unresolved"]
        ))
        self.assertFalse(any(
            row["symbol"] == "code_match_line" for row in contract["unresolved"]
        ))
        cjson = next(
            unit for unit in contract["descriptor_support_units"]
            if unit["path"] == "src/modules/db2/support/cjson.c"
        )
        self.assertEqual(cjson["resolves"], sorted(checker.CJSON_BASE_REFERENCES))
        self.assertEqual(cjson["defines"], checker.CJSON_DEFINES)
        random_support = next(
            unit for unit in contract["descriptor_support_units"]
            if unit["path"] == "src/modules/db2/support/random_primitives.c"
        )
        self.assertEqual(
            random_support["defines"], ["platform_random_bytes", "platform_random_hex"]
        )
        self.assertEqual(random_support["resolves"], random_support["defines"])
        seed_support = next(
            unit for unit in contract["descriptor_support_units"]
            if unit["path"] == "src/modules/db2/support/rel_seed_primitives.c"
        )
        self.assertEqual(seed_support["defines"], [
            "rel_types_seed_at", "rel_types_seed_count", "rel_types_seed_lookup",
        ])
        self.assertEqual(seed_support["resolves"], seed_support["defines"])
        code_match_support = next(
            unit for unit in contract["descriptor_support_units"]
            if unit["path"] == "src/modules/db2/support/code_match_primitives.c"
        )
        self.assertEqual(code_match_support["defines"], ["code_match_line"])
        self.assertEqual(code_match_support["resolves"], ["code_match_line"])
        self.assertEqual(code_match_support["allowed_undefined"], ["strncmp", "strstr"])
        self.assertEqual(
            code_match_support["source_sha256"],
            hashlib.sha256((REPO / code_match_support["path"]).read_bytes()).hexdigest(),
        )
        self.assertIn(
            code_match_support["path"],
            json.loads((REPO / checker.DESCRIPTOR).read_text(encoding="utf-8"))["sources"],
        )


if __name__ == "__main__":
    unittest.main()
