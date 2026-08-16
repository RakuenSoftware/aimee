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
        self.temp.cleanup()

    def _contract(self) -> dict[str, object]:
        sources = ["src/modules/db2/c/a.c"]
        result: dict[str, object] = {
            "schema_version": 1,
            "module": "db2",
            "source_revision": "a" * 40,
            "source_fingerprint": checker.source_fingerprint(self.root, sources),
            "probe": {
                "compile_driver": "src/Makefile",
                "extra_c_flags": checker.PROBE_FLAGS,
                "link_mode": "relocatable-no-libraries",
                "helper_objects": [],
                "libraries": [],
            },
            "translation_units": sources,
            "unresolved": [{
                "symbol": "outside",
                "references": sources,
                "disposition": "portable-core-promotion",
                "evidence": "Fixture-owned unresolved support dependency.",
            }],
            "summary": {
                "translation_units": 1,
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
        descriptor = {"contracts": [checker.CONTRACT.as_posix()]}
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
            "unresolved_symbols": len(rows),  # type: ignore[arg-type]
            "dispositions": counts,
        }
        self._fingerprint(value)

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

    def test_real_repository_contract_metadata(self) -> None:
        checker.check(REPO, run_probe=False)


if __name__ == "__main__":
    unittest.main()
