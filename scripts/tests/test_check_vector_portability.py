#!/usr/bin/env python3
"""Failure-mode tests for the DB3 portability audit."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts/check_vector_portability.py"
SPEC = importlib.util.spec_from_file_location("check_vector_portability", CHECKER)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class PortabilityTests(unittest.TestCase):
    def audit(self) -> dict[str, object]:
        return json.loads((REPO_ROOT / checker.AUDIT).read_text(encoding="utf-8"))

    def ledger(self) -> dict[str, object]:
        return json.loads((REPO_ROOT / checker.LEDGER).read_text(encoding="utf-8"))

    def assert_rule(self, mutate, rule: str, *, ledger: bool = False) -> None:
        audit_value = copy.deepcopy(self.audit())
        ledger_value = copy.deepcopy(self.ledger())
        mutate(ledger_value if ledger else audit_value)
        with self.assertRaisesRegex(checker.PortabilityError, rf"rule={rule}"):
            checker.validate(audit_value, ledger_value)

    def test_production_audit_covers_every_pgvector_declaration(self) -> None:
        summary = checker.run(REPO_ROOT)
        self.assertEqual(sum(summary.values()), 76)
        self.assertEqual(summary, {
            # All fourteen, and all portable now: search version 2 carries a
            # collection and a conjunction of eq/ne/in predicates over labels
            # that may hold several values, which is every filter they apply.
            "portable-search": 14,
            "committed-mutation": 32,
            "provider-control": 13,
            # Two more than before wiring the route: pgvec_memory_point_visible,
            # which reads the canonical scope tables to decide whether a
            # PROVIDER's answer may be shown -- handing that to a provider would
            # ask the untrusted party to certify itself -- and
            # pgvec_memory_vector_routed_searches, process-local state whose
            # whole value is being measured on this side.
            "db2-authority": 14,
            "portable-analytics": 3,
        })
        source = checker.source_symbols(self.ledger())
        classified = [
            symbol
            for group in self.audit()["classifications"]
            for symbol in group["symbols"]
        ]
        self.assertEqual(sorted(classified), source)
        # By id, not by position. Inserting a group shifts every index after it,
        # so a positional assertion silently starts checking its neighbour --
        # which is exactly what happened when portable-search was split.
        groups = {group["id"]: group["symbols"]
                  for group in self.audit()["classifications"]}
        self.assertIn("pgvec_schema_version", groups["db2-authority"])
        # The two families, named by a member of each. Currency: code and kb
        # searches JOIN projects for lifecycle_state and current_generation.
        # Scope membership: memory searches decide visibility from rows in
        # memory_scopes and memory_workspaces. Neither is an attribute of a
        # point, which is what DB3 v1 filters on.
        for symbol in ("pgvec_code_search", "pgvec_kb_search",
                       "pgvec_memory_vector_search_record_type",
                       "pgvec_memory_vector_search_with_kinds"):
            self.assertIn(symbol, groups["portable-search"])

    def test_root_identity_and_closed_classifications(self) -> None:
        cases = (
            (lambda value: value.__setitem__("extra", 1), "keys"),
            (lambda value: value.__setitem__("schema_version", True), "schema-version"),
            (lambda value: value.__setitem__("module", "db2"), "module"),
            (lambda value: value.__setitem__("source", "other.json"), "source"),
            (lambda value: value.__setitem__("classifications", []), "classifications"),
            (lambda value: value["classifications"][0].__setitem__("id", "search"),
             "classification-identity"),
            (lambda value: value["classifications"][0].__setitem__("disposition", "maybe"),
             "classification-identity"),
            (lambda value: value["classifications"][0].__setitem__("rationale", ""),
             "string"),
            (lambda value: value["classifications"][0].__setitem__("extra", 1), "keys"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_missing_extra_duplicate_and_unsorted_symbols_fail_closed(self) -> None:
        self.assert_rule(
            lambda value: value["classifications"][0]["symbols"].pop(), "coverage")
        self.assert_rule(
            lambda value: value["classifications"][0]["symbols"].append("pgvec_not_real"),
            "coverage")
        self.assert_rule(
            lambda value: value["classifications"][1]["symbols"].append(
                value["classifications"][0]["symbols"][0]),
            "symbol-order")
        self.assert_rule(
            lambda value: value["classifications"][0]["symbols"].reverse(), "symbol-order")
        self.assert_rule(
            lambda value: value["classifications"][0]["symbols"].__setitem__(0, "not_pgvec"),
            "symbol")

    def test_duplicate_classification_is_rejected_after_sorting(self) -> None:
        def mutate(value: dict[str, object]) -> None:
            groups = value["classifications"]
            duplicate = groups[0]["symbols"][0]
            groups[1]["symbols"].append(duplicate)
            groups[1]["symbols"].sort()
        self.assert_rule(mutate, "symbol-duplicate")

    def test_ledger_addition_removal_and_duplicate_are_rejected(self) -> None:
        def add(value: dict[str, object]) -> None:
            row = copy.deepcopy(value["declarations"][0])
            row["symbol"] = "pgvec_new_operation"
            value["declarations"].append(row)
        self.assert_rule(add, "source-fingerprint", ledger=True)

        def remove(value: dict[str, object]) -> None:
            value["declarations"] = [
                row for row in value["declarations"]
                if row["symbol"] != "pgvec_code_search"
            ]
        self.assert_rule(remove, "source-fingerprint", ledger=True)

        def duplicate(value: dict[str, object]) -> None:
            row = next(row for row in value["declarations"]
                       if row["symbol"] == "pgvec_code_search")
            value["declarations"].append(copy.deepcopy(row))
        self.assert_rule(duplicate, "ledger-duplicate", ledger=True)

    def test_source_fingerprint_is_an_independent_gate(self) -> None:
        self.assert_rule(
            lambda value: value.__setitem__("source_symbols_sha256", "0" * 64),
            "source-fingerprint")

    def test_reference_route_import_boundary_is_machine_enforced(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in (checker.AUDIT, checker.LEDGER, checker.ROUTE_SOURCE):
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPO_ROOT / relative, target)
            checker.run(root)
            route = root / checker.ROUTE_SOURCE
            route.write_text("#include <stdint.h>\n" + route.read_text(encoding="utf-8"),
                             encoding="utf-8")
            checker.run(root)
            route.write_text('#include "c/pgvec_transport.h"\n' +
                             route.read_text(encoding="utf-8"), encoding="utf-8")
            with self.assertRaisesRegex(checker.PortabilityError, "rule=route-boundary"):
                checker.run(root)
            original = (REPO_ROOT / checker.ROUTE_SOURCE).read_text(encoding="utf-8")
            for directive in ('# include "c/pgvec_transport.h"\n',
                              '#/**/include "c/pgvec_transport.h"\n'):
                route.write_text(directive + original, encoding="utf-8")
                with self.assertRaisesRegex(checker.PortabilityError, "rule=route-boundary"):
                    checker.run(root)

    def test_malformed_json_and_resource_limits_are_typed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "input.json"
            path.write_text('{"a":1,"a":2}', encoding="utf-8")
            with self.assertRaisesRegex(checker.PortabilityError, "rule=json-duplicate-key"):
                checker.load_json(path)
            path.write_text('{"a":NaN}', encoding="utf-8")
            with self.assertRaisesRegex(checker.PortabilityError, "rule=json-number-domain"):
                checker.load_json(path)
            path.write_bytes(b"x" * (checker.MAX_BYTES + 1))
            with self.assertRaisesRegex(checker.PortabilityError, "rule=input-size"):
                checker.load_json(path)

    def test_cli_checks_a_copied_repository_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in (checker.AUDIT, checker.LEDGER, checker.ROUTE_SOURCE):
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPO_ROOT / relative, target)
            result = subprocess.run(
                [sys.executable, "-I", "-S", str(CHECKER), "--root", str(root)],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("total=76", result.stdout)
            fingerprint = subprocess.run(
                [sys.executable, "-I", "-S", str(CHECKER), "--root", str(root),
                 "--print-source-fingerprint"],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(fingerprint.returncode, 0, fingerprint.stderr)
            self.assertEqual(
                fingerprint.stdout.strip(),
                f"{self.audit()['source_symbols_sha256']}  pgvec-symbols=76",
            )


if __name__ == "__main__":
    unittest.main()
