#!/usr/bin/env python3
"""Failure-mode and reproducibility tests for the DB2 catalog generator."""

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
GENERATOR = REPO_ROOT / "scripts/gen_db2_contract.py"
SPEC = importlib.util.spec_from_file_location("gen_db2_contract", GENERATOR)
assert SPEC and SPEC.loader
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class ContractTests(unittest.TestCase):
    def catalog(self) -> dict[str, object]:
        return json.loads((REPO_ROOT / generator.CATALOG).read_text(encoding="utf-8"))

    def assert_rule(self, mutate, rule: str) -> None:
        value = copy.deepcopy(self.catalog())
        mutate(value)
        with self.assertRaisesRegex(generator.ContractError, rf"rule={rule}"):
            generator.validate_catalog(value)

    def fixture(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        for relative in (
            generator.CATALOG,
            generator.DESCRIPTOR,
            generator.PROCESS_CONTRACTS,
            generator.HEADER,
            generator.CLIENT_HEADER,
            generator.CLIENT_SOURCE,
            generator.GO_CONTRACT,
            generator.BASELINE,
            generator.DECLARATION_REVIEW,
            generator.DECLARATION_LEDGER,
        ):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / relative, target)
        return temporary

    def test_production_catalog_and_generated_outputs_match(self) -> None:
        generator.run(REPO_ROOT, False)
        catalog = generator.validate_catalog(generator.load_json(REPO_ROOT / generator.CATALOG))
        header, client_header, client_source, go_contract, baseline = generator.generated(REPO_ROOT)
        self.assertEqual(header, (REPO_ROOT / generator.HEADER).read_bytes())
        self.assertEqual(client_header, (REPO_ROOT / generator.CLIENT_HEADER).read_bytes())
        self.assertEqual(client_source, (REPO_ROOT / generator.CLIENT_SOURCE).read_bytes())
        self.assertEqual(go_contract, (REPO_ROOT / generator.GO_CONTRACT).read_bytes())
        self.assertEqual(baseline, (REPO_ROOT / generator.BASELINE).read_bytes())
        fingerprint = generator.catalog_fingerprint(catalog)
        self.assertIn(fingerprint.encode(), header)
        self.assertIn(b"#define AIMEE_DB2_RESULT_INVALID_STATE 5u", header)
        self.assertIn(b"aimee_db2_request_header_decode", header)
        self.assertIn(b"AIMEE_DB2_ENVELOPE_HEADER_LEN", header)
        self.assertIn(b"aimee_db2_health_call", client_header)
        self.assertIn(b"AIMEE_MODULE_CALL_PROTOCOL", client_source)
        self.assertIn(fingerprint.encode(), go_contract)
        self.assertIn(b"func DecodeHealthResponse", go_contract)
        self.assertIn(b"func DecodeRequestHeader", go_contract)
        self.assertIn(b"func DecodeReplyHeader", go_contract)
        self.assertIn(b"ErrMalformedHealth", go_contract)
        self.assertIn(b"ResultOK", go_contract)
        self.assertIn(b"HealthFlagPGTrgm", go_contract)
        self.assertIn(b"HealthFlagKBTables", go_contract)
        self.assertEqual(json.loads(baseline)["catalog_sha256"], fingerprint)
        self.assertEqual(
            json.loads(baseline)["result_codes"],
            [{"id": index, "name": name} for index, name in enumerate(generator.RESULT_CODES)],
        )

    def test_additive_body_envelope_vectors_are_closed_and_fixed_width(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        envelope = baseline["body_envelope"]
        self.assertEqual(envelope["header_len"], generator.ENVELOPE_HEADER_LEN)
        request = bytes.fromhex(envelope["request"]["positive"])
        self.assertEqual(len(request), generator.ENVELOPE_HEADER_LEN + 3)
        self.assertEqual(int.from_bytes(request[0:4], "little"),
                         generator.ENVELOPE_REQUEST_MAGIC)
        self.assertEqual(int.from_bytes(request[6:8], "little"),
                         generator.ENVELOPE_HEADER_LEN)
        self.assertEqual(int.from_bytes(request[16:20], "little"), 3)
        self.assertEqual(
            [row["mutation"] for row in envelope["request"]["negative"]],
            ["bad_magic", "bad_version", "bad_header_len", "zero_operation",
             "payload_length", "reserved", "short", "long"],
        )
        self.assertEqual(
            [row["result"] for row in envelope["reply"]["positive"]],
            list(range(len(generator.RESULT_CODES))),
        )
        self.assertEqual(
            [row["mutation"] for row in envelope["reply"]["negative"]],
            ["bad_magic", "bad_version", "bad_header_len", "zero_operation",
             "unknown_result", "payload_length", "reserved", "short", "long"],
        )

    def test_wire_vectors_cover_every_flag_and_closed_failure_fields(self) -> None:
        baseline = json.loads((REPO_ROOT / generator.BASELINE).read_text(encoding="utf-8"))
        operation = baseline["operations"][0]
        self.assertEqual([row["flags"] for row in operation["reply"]["positive"]], list(range(8)))
        self.assertEqual(
            [row["mutation"] for row in operation["request"]["negative"]],
            ["bad_magic", "bad_version", "short", "long"],
        )
        self.assertEqual(
            [row["mutation"] for row in operation["reply"]["negative"]],
            ["bad_magic", "bad_version", "unknown_flags", "reserved", "short", "long"],
        )
        self.assertTrue(all(len(bytes.fromhex(row["hex"])) == 16
                            for row in operation["reply"]["positive"]))

    def test_root_and_version_mutations(self) -> None:
        cases = (
            (lambda value: value.__setitem__("extra", 1), "keys"),
            (lambda value: value.__setitem__("schema_version", 2), "schema-version"),
            (lambda value: value.__setitem__("module", "db3"), "module"),
            (lambda value: value.__setitem__("wire_version", True), "wire-version"),
            (lambda value: value.__setitem__("catalog_complete", 1), "catalog-complete-type"),
            (lambda value: value.__setitem__("catalog_complete", True), "catalog-complete"),
            (lambda value: value["body_envelope"].__setitem__("request_magic", 1),
             "body-envelope"),
            (lambda value: value["body_envelope"].__setitem__("reply_magic", 1),
             "body-envelope"),
            (lambda value: value["body_envelope"].__setitem__("header_len", 23),
             "body-envelope"),
            (lambda value: value["body_envelope"].__setitem__("extra", 1), "keys"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_family_mutations(self) -> None:
        cases = (
            (lambda value: value["families"].pop(), "families"),
            (lambda value: value["families"][0].__setitem__("name", "tenancy"),
             "family-order"),
            (lambda value: value["families"][0].__setitem__("id", 2), "family-id"),
            (lambda value: value["families"][0].__setitem__("event_kind", 11522),
             "family-event-kind"),
            (lambda value: value["families"][1].__setitem__("active", True),
             "family-active"),
            (lambda value: value["families"][0].__setitem__("extra", 1), "keys"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_operation_identity_and_semantic_mutations(self) -> None:
        cases = (
            (lambda value: value.__setitem__("result_codes", ["ok"]), "result-codes"),
            (lambda value: value.__setitem__("operations", []), "operations"),
            (lambda value: value["operations"][0].__setitem__("family", "unknown"),
             "operation-family"),
            (lambda value: value["operations"][0].__setitem__("id", True), "integer"),
            (lambda value: value["operations"][0].__setitem__("name", "Health"),
             "operation-name"),
            (lambda value: value["operations"][0].__setitem__("wire_format", "raw-sql"),
             "unsupported-operation"),
            (lambda value: value["operations"][0].__setitem__("scope", "tenant"),
             "operation-semantics"),
            (lambda value: value["operations"][0].__setitem__("results", ["retryable"]),
             "operation-results"),
            (lambda value: value["operations"][0].__setitem__("db3_placement", "eligible"),
             "db3-placement"),
            (lambda value: value["operations"][0].__setitem__("extra", 1), "keys"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_operation_duplicates_and_order_are_rejected(self) -> None:
        self.assert_rule(
            lambda value: value["operations"].append(copy.deepcopy(value["operations"][0])),
            "operation-duplicate",
        )
        self.assert_rule(
            lambda value: value["operations"].append({
                **copy.deepcopy(value["operations"][0]),
                "id": 2,
                "name": "health_second",
            }),
            "unsupported-operation",
        )

    def test_health_wire_shape_mutations(self) -> None:
        cases = (
            (lambda value: value["operations"][0]["request"].__setitem__("magic", 1),
             "health-request"),
            (lambda value: value["operations"][0]["request"].__setitem__("encoded_size", 9),
             "health-request"),
            (lambda value: value["operations"][0]["reply"].__setitem__("magic", 1),
             "health-reply"),
            (lambda value: value["operations"][0]["reply"].__setitem__("encoded_size", 15),
             "health-reply"),
            (lambda value: value["operations"][0]["reply"]["flags"][0].__setitem__("bit", True),
             "integer"),
            (lambda value: value["operations"][0]["reply"]["flags"][0].__setitem__("name", "x"),
             "health-flags"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_rule(mutate, rule)

    def test_descriptor_and_process_bindings_fail_closed(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            descriptor_path = root / generator.DESCRIPTOR
            descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
            descriptor["contracts"] = []
            descriptor_path.write_text(json.dumps(descriptor), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=descriptor-ownership"):
                generator.generated(root)

            shutil.copy2(REPO_ROOT / generator.DESCRIPTOR, descriptor_path)
            process_path = root / generator.PROCESS_CONTRACTS
            process = json.loads(process_path.read_text(encoding="utf-8"))
            db2 = next(row for row in process["components"] if row["id"] == "db2")
            db2["stages"][0]["event_kind"] = 11522
            process_path.write_text(json.dumps(process), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=process-activation"):
                generator.generated(root)
        finally:
            temporary.cleanup()

    def test_reserved_event_kind_collision_is_rejected(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            path = root / generator.PROCESS_CONTRACTS
            process = json.loads(path.read_text(encoding="utf-8"))
            component = next(row for row in process["components"] if row.get("stages"))
            component["stages"][0]["event_kind"] = 11528
            path.write_text(json.dumps(process), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=event-kind-collision"):
                generator.generated(root)
        finally:
            temporary.cleanup()

    def test_declaration_completeness_gate_fails_closed(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            review_path = root / generator.DECLARATION_REVIEW
            review = json.loads(review_path.read_text(encoding="utf-8"))
            review["declarations_complete"] = True
            review_path.write_text(json.dumps(review), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError,
                                        "rule=declaration-completeness-drift"):
                generator.generated(root)

            shutil.copy2(REPO_ROOT / generator.DECLARATION_REVIEW, review_path)
            catalog = generator.validate_catalog(
                generator.load_json(root / generator.CATALOG))
            catalog["catalog_complete"] = True
            with self.assertRaisesRegex(generator.ContractError,
                                        "rule=catalog-declaration-gate"):
                generator._validate_declaration_gate(root, catalog)
        finally:
            temporary.cleanup()

    def test_write_then_check_and_drift_detection(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            (root / generator.HEADER).unlink()
            (root / generator.CLIENT_HEADER).unlink()
            (root / generator.CLIENT_SOURCE).unlink()
            (root / generator.GO_CONTRACT).unlink()
            (root / generator.BASELINE).unlink()
            generator.run(root, True)
            generator.run(root, False)
            (root / generator.HEADER).write_text("drift\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=generated-drift"):
                generator.run(root, False)
            generator.run(root, True)
            (root / generator.GO_CONTRACT).write_text("package drift\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=generated-drift"):
                generator.run(root, False)
        finally:
            temporary.cleanup()

    def test_output_symlink_is_rejected(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            header = root / generator.HEADER
            header.unlink()
            header.symlink_to(root / generator.BASELINE)
            with self.assertRaisesRegex(generator.ContractError, "rule=output-symlink"):
                generator.run(root, True)
        finally:
            temporary.cleanup()

    def test_json_input_failures_are_typed(self) -> None:
        cases = (
            (b'{"x":1,"x":2}', "json-duplicate-key"),
            (b"\xef\xbb\xbf{}", "json-bom"),
            (b'{"x":1.5}', "json-number-domain"),
            (b'{"x":NaN}', "json-number-domain"),
            (b"\xff", "json-encoding"),
            (b"{", "json-parse"),
        )
        for raw, rule in cases:
            with tempfile.TemporaryDirectory() as tmp, self.subTest(rule=rule):
                path = Path(tmp) / "input.json"
                path.write_bytes(raw)
                with self.assertRaisesRegex(generator.ContractError, rf"rule={rule}"):
                    generator.load_json(path)

    def test_json_resource_limits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input.json"
            path.write_bytes(b" " * (generator.MAX_BYTES + 1))
            with self.assertRaisesRegex(generator.ContractError, "rule=input-size"):
                generator.load_json(path)
            path.write_text("[" * (generator.MAX_DEPTH + 2) + "0" +
                            "]" * (generator.MAX_DEPTH + 2), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=json-depth"):
                generator.load_json(path)
            path.write_text(json.dumps([0] * (generator.MAX_ARRAY + 1)), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=json-array-size"):
                generator.load_json(path)

    def test_cli_is_cwd_independent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, "-I", "-S", str(GENERATOR)],
                cwd=tmp,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gen_db2_contract: ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
