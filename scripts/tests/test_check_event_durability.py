#!/usr/bin/env python3
"""Unit tests for the fail-closed event durability coverage gate."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_event_durability", ROOT / "scripts/check_event_durability.py")
assert SPEC and SPEC.loader
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class DurabilityCheckTest(unittest.TestCase):
    def fixture(self, *, kind: int = 5889, durability: str | None = "ledger",
                header: str = "#define AIMEE_MEMORY_EVENT_TEST 5889u\n",
                runtime: set[int] | None = None) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        stage: dict[str, object] = {
            "id": 1, "name": "test", "event_kind": kind,
            "durability_reason": "unit-test disposition",
        }
        if durability is not None:
            stage["durability"] = durability
        if durability in {"ledger", "sampled"}:
            stage["emitter"] = "obs_bus_emit_durable_event"
        if durability == "sampled":
            stage["sample_rate"] = 0.25
        contract = {"components": [{"id": "memory", "execution": "process", "stages": [stage]}]}
        contract_path = root / CHECK.CONTRACTS
        contract_path.parent.mkdir(parents=True)
        contract_path.write_text(json.dumps(contract), encoding="utf-8")
        header_path = root / "src/modules/memory/include/aimee/memory/module_api.h"
        header_path.parent.mkdir(parents=True)
        header_path.write_text(header, encoding="utf-8")
        rows = runtime if runtime is not None else ({kind} if durability == "ledger" else set())
        obs_path = root / CHECK.OBS_BUS
        obs_path.parent.mkdir(parents=True)
        obs_path.write_text(
            "static const int LEDGER_EVENT_KINDS[] = {\n" +
            "".join(f'  {{{value}u, "test"}},\n' for value in sorted(rows)) +
            "};\n"
            'obs_bus_emit_durable_event("bus.module.request", "", "", "");\n'
            'obs_bus_emit_durable_event("bus.module.reply", "", "", "");\n',
            encoding="utf-8")
        return root

    def test_valid_contract_reports_real_count(self) -> None:
        counts, errors = CHECK.analyse(self.fixture())
        self.assertEqual(errors, [])
        self.assertEqual(counts["header_kinds"], 1)
        self.assertEqual(counts["ledger"], 1)

    def test_reserved_header_kind_has_a_durability_declaration(self) -> None:
        root = self.fixture()
        path = root / CHECK.CONTRACTS
        contract = json.loads(path.read_text(encoding="utf-8"))
        component = contract["components"][0]
        component["durability_declarations"] = component.pop("stages")
        component["stages"] = []
        path.write_text(json.dumps(contract), encoding="utf-8")
        counts, errors = CHECK.analyse(root)
        self.assertEqual(errors, [])
        self.assertEqual(counts["declared"], 1)
        self.assertEqual(counts["ledger"], 1)

    def test_missing_durability_fails(self) -> None:
        _, errors = CHECK.analyse(self.fixture(durability=None))
        self.assertTrue(any("missing or invalid durability" in error for error in errors))

    def test_header_kind_without_contract_fails(self) -> None:
        _, errors = CHECK.analyse(self.fixture(header="#define AIMEE_MEMORY_EVENT_TEST 5890u\n"))
        self.assertTrue(any("undeclared kind 5890" in error for error in errors))

    def test_ledger_kind_without_runtime_emitter_fails(self) -> None:
        _, errors = CHECK.analyse(self.fixture(runtime=set()))
        self.assertTrue(any("absent from the runtime emitter table" in error for error in errors))

    def test_sampled_kind_without_runtime_sampler_fails(self) -> None:
        _, errors = CHECK.analyse(self.fixture(durability="sampled"))
        self.assertTrue(any("absent from the runtime sampler table" in error for error in errors))

    def test_zero_resolved_kinds_is_never_success(self) -> None:
        _, errors = CHECK.analyse(self.fixture(header="#define NOT_AN_EVENT 5889u\n"))
        self.assertTrue(any("zero module_api event kinds resolved" in error for error in errors))

    def test_repository_contract(self) -> None:
        counts, errors = CHECK.analyse(ROOT)
        self.assertEqual(errors, [])
        self.assertGreater(counts["header_kinds"], 0)


if __name__ == "__main__":
    unittest.main()
