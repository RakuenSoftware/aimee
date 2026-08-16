#!/usr/bin/env python3
"""Failure-mode and reproducibility tests for the DB3 protocol generator."""

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


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts/gen_db3_contract.py"
SPEC = importlib.util.spec_from_file_location("gen_db3_contract", GENERATOR)
assert SPEC and SPEC.loader
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class ContractTests(unittest.TestCase):
    def catalog(self) -> dict[str, object]:
        return json.loads((ROOT / generator.CATALOG).read_text(encoding="utf-8"))

    def registry(self) -> dict[str, object]:
        return json.loads((ROOT / generator.REGISTRY).read_text(encoding="utf-8"))

    def assert_catalog_rule(self, mutate, rule: str) -> None:
        value = copy.deepcopy(self.catalog())
        mutate(value)
        with self.assertRaisesRegex(generator.ContractError, rf"rule={rule}"):
            generator.validate_catalog(value)

    def fixture(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        for relative in (
            generator.REGISTRY, generator.CATALOG, generator.DESCRIPTOR,
            generator.PROCESS_CONTRACTS, generator.DB1_CATALOG, generator.DB2_CATALOG,
            generator.HEADER, generator.GO_CONTRACT, generator.BASELINE,
        ):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        return temporary

    def test_production_outputs_are_reproducible(self) -> None:
        generator.run(ROOT, False)
        header, go_contract, baseline = generator.generated(ROOT)
        self.assertEqual(header, (ROOT / generator.HEADER).read_bytes())
        self.assertEqual(go_contract, (ROOT / generator.GO_CONTRACT).read_bytes())
        self.assertEqual(baseline, (ROOT / generator.BASELINE).read_bytes())
        parsed = json.loads(baseline)
        self.assertEqual(parsed["protocol_id"], 3)
        self.assertEqual([row["event_kind"] for row in parsed["events"]],
                         [0x80030001, 0x80030002, 0x80030003, 0x80030004, 0x80030005])
        self.assertIn(b"AIMEE_DB3_EVENT_APPLY", header)
        self.assertIn(b"func DecodeSearchRequest", go_contract)
        self.assertIn(parsed["contract_sha256"].encode(), header)
        self.assertIn(parsed["contract_sha256"].encode(), go_contract)

    def test_registry_namespace_and_allocations_fail_closed(self) -> None:
        cases = (
            (lambda value: value.__setitem__("schema_version", 2), "registry-version"),
            (lambda value: value["namespace"].__setitem__("kind_flag", 0), "registry-namespace"),
            (lambda value: value["protocols"][0].__setitem__("id", 0), "integer"),
            (lambda value: value["protocols"][0].__setitem__("id", 4), "registry-db3"),
            (lambda value: value["protocols"].append(copy.deepcopy(value["protocols"][0])),
             "registry-duplicate"),
            (lambda value: value["protocols"][0].__setitem__("catalog", "elsewhere"),
             "registry-db3"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                value = copy.deepcopy(self.registry())
                mutate(value)
                with self.assertRaisesRegex(generator.ContractError, rf"rule={rule}"):
                    generator.validate_registry(value)

    def test_catalog_identity_event_and_wire_mutations(self) -> None:
        cases = (
            (lambda value: value.__setitem__("extra", 1), "keys"),
            (lambda value: value.__setitem__("schema_version", 2), "catalog-version"),
            (lambda value: value.__setitem__("protocol_id", 4), "catalog-identity"),
            (lambda value: value["events"].pop(), "events"),
            (lambda value: value["events"][1].__setitem__("delivery", "one-provider"),
             "event-semantics"),
            (lambda value: value["events"][3].__setitem__("pattern", "notification"),
             "event-semantics"),
            (lambda value: value["limits"].__setitem__("dimension", 8192), "limits"),
            (lambda value: value["wire"]["search_request"].__setitem__("header_bytes", 35),
             "search-request-wire"),
            (lambda value: value["wire"]["search_reply"].__setitem__("candidate_bytes", 8),
             "search-reply-wire"),
            (lambda value: value["wire"]["apply"].__setitem__("magic", 1), "apply-wire"),
        )
        for mutate, rule in cases:
            with self.subTest(rule=rule):
                self.assert_catalog_rule(mutate, rule)

    def test_event_formula_rejects_zero_and_capacity_overflow(self) -> None:
        namespace, _ = generator.validate_registry(self.registry())
        self.assertEqual(generator.event_kind(namespace, 3, 1), 0x80030001)
        for protocol_id, event_id, rule in (
            (0, 1, "event-id-zero"), (3, 0, "event-id-zero"),
            (0x8000, 1, "event-capacity"), (3, 0x10000, "event-capacity"),
        ):
            with self.subTest(protocol_id=protocol_id, event_id=event_id):
                with self.assertRaisesRegex(generator.ContractError, rf"rule={rule}"):
                    generator.event_kind(namespace, protocol_id, event_id)

    def test_repository_ownership_and_collision_checks(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            descriptor_path = root / generator.DESCRIPTOR
            descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
            descriptor["contracts"].remove(generator.CATALOG.as_posix())
            descriptor_path.write_text(json.dumps(descriptor), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=descriptor-ownership"):
                generator.generated(root)

            shutil.copy2(ROOT / generator.DESCRIPTOR, descriptor_path)
            process_path = root / generator.PROCESS_CONTRACTS
            process = json.loads(process_path.read_text(encoding="utf-8"))
            process["components"][0]["stages"] = [{
                "id": 1, "name": "collision", "event_kind": 0x80030001,
            }]
            process_path.write_text(json.dumps(process), encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=event-kind-collision"):
                generator.generated(root)
        finally:
            temporary.cleanup()

    def test_retired_db3_event_literals_are_rejected_inside_db2(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            stale = root / "src/modules/db2/stale.c"
            stale.write_text("unsigned event = 11780u;\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=stale-event-kind"):
                generator.generated(root)
            stale.write_text("unsigned event = 0x2e04u;\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=stale-event-kind"):
                generator.generated(root)
        finally:
            temporary.cleanup()

    def test_write_check_drift_and_symlink_rejection(self) -> None:
        temporary = self.fixture()
        try:
            root = Path(temporary.name)
            for relative in (generator.HEADER, generator.GO_CONTRACT, generator.BASELINE):
                (root / relative).unlink()
            generator.run(root, True)
            generator.run(root, False)
            (root / generator.GO_CONTRACT).write_text("package drift\n", encoding="utf-8")
            with self.assertRaisesRegex(generator.ContractError, "rule=generated-drift"):
                generator.run(root, False)
            generator.run(root, True)
            header = root / generator.HEADER
            header.unlink()
            header.symlink_to(root / generator.BASELINE)
            with self.assertRaisesRegex(generator.ContractError, "rule=output-symlink"):
                generator.run(root, True)
        finally:
            temporary.cleanup()

    def test_json_parser_rejects_ambiguous_or_unbounded_input(self) -> None:
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

    def test_cli_is_cwd_independent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, "-I", "-S", str(GENERATOR)], cwd=tmp, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gen_db3_contract: ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
