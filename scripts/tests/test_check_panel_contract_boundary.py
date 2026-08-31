#!/usr/bin/env python3
"""Mutation tests for the required/optional panel contract boundary."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO / "scripts/check_panel_contract_boundary.py"
SPEC = importlib.util.spec_from_file_location("panel_contract_boundary", CHECKER_PATH)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


class PanelContractBoundaryTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        paths = set(checker.TEMPORARY_TYPE_CONSUMERS)
        paths |= set(checker.TEMPORARY_ENSEMBLE_CONSUMERS)
        paths.add("src/server/server_pipeline.c")
        paths |= {
            "src/modules/roundtable/roundtable_types.h",
            "src/tests/test_delegate_ensemble.c",
        }
        for relative in paths:
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            source = REPO / relative
            if source.is_file():
                shutil.copy2(source, target)
            else:
                target.write_text("/* fixture */\n", encoding="utf-8")

    def mutate_rejected(self, relative: str, include: str, rule: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            path = root / relative
            path.write_text(path.read_text(encoding="utf-8") + f'\n#include "{include}"\n',
                            encoding="utf-8")
            with self.assertRaisesRegex(checker.CheckError, f"rule={rule}"):
                checker.validate(root)

    def test_repository_and_fixture_pass(self) -> None:
        checker.validate(REPO)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            checker.validate(root)

    def test_optional_type_header_leak_is_rejected(self) -> None:
        self.mutate_rejected(
            "src/headers/evidence_replay.h",
            "roundtable_types.h",
            "optional-type-header-leak",
        )

    def test_ensemble_consumer_is_rejected(self) -> None:
        self.mutate_rejected(
            "src/server/server_pipeline.c",
            "delegate_ensemble.h",
            "optional-ensemble-header-leak",
        )

    def test_owner_and_test_private_includes_are_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            checker.validate(root)

    def test_known_debt_disappearing_requires_ratchet_update(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            path = root / "src/cmd_agent_delegate.c"
            path.write_text(path.read_text(encoding="utf-8").replace(
                '#include "delegate_ensemble.h"',
                '#include <aimee/delegates/panel_provider.h>',
                1,
            ), encoding="utf-8")
            with self.assertRaisesRegex(checker.CheckError,
                                        "rule=temporary-ensemble-header-debt"):
                checker.validate(root)


if __name__ == "__main__":
    unittest.main()
