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
CHECKER = REPO_ROOT / "scripts/check_db3_portability.py"
SPEC = importlib.util.spec_from_file_location("check_db3_portability", CHECKER)
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
            "portable-search": 14,
            "committed-mutation": 32,
            "provider-control": 15,
            "db2-authority": 12,
            "portable-analytics": 3,
        })
        source = checker.source_symbols(self.ledger())
        classified = [
            symbol
            for group in self.audit()["classifications"]
            for symbol in group["symbols"]
        ]
        self.assertEqual(sorted(classified), source)
        self.assertIn("pgvec_memory_vector_search_record_type",
                      self.audit()["classifications"][0]["symbols"])
        self.assertIn("pgvec_schema_version",
                      self.audit()["classifications"][3]["symbols"])

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

