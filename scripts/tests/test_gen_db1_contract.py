#!/usr/bin/env python3
"""Failure modes of the DB1 operation-catalog contract."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SPEC = importlib.util.spec_from_file_location(
    "gen_db1_contract", REPO_ROOT / "scripts/gen_db1_contract.py")
contract = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(contract)


def sandbox() -> tempfile.TemporaryDirectory[str]:
    """A copy of the three files the contract binds together."""
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    for relative in (contract.CATALOG, contract.HEADER, contract.PROCESS_CONTRACTS):
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO_ROOT / relative, target)
    return tmp


class CatalogTests(unittest.TestCase):
    def catalog(self, root: Path) -> dict:
        return json.loads((root / contract.CATALOG).read_text(encoding="utf-8"))

    def write(self, root: Path, catalog: dict) -> None:
        (root / contract.CATALOG).write_text(json.dumps(catalog, indent=2) + "\n",
                                             encoding="utf-8")

    def reserved(self, catalog: dict) -> dict:
        """The first still-reserved family.

        Looked up rather than indexed: activating a family is the whole point of
        this catalog, and an index would quietly retarget these tests at an
        active one the moment that happened.
        """
        for family in catalog["families"]:
            if not family["active"]:
                return family
        self.skipTest("no reserved family remains to exercise")

    def assertRule(self, root: Path, rule: str) -> None:
        with self.assertRaises(contract.ContractError) as caught:
            contract.run(root)
        self.assertIn(f"rule={rule}", str(caught.exception))

    def test_the_shipped_catalog_validates(self) -> None:
        contract.run(REPO_ROOT)

    def test_every_family_kind_is_carved_from_the_principal_ref(self) -> None:
        # The kinds are not free: 4096 + 30*256 + family id. If this drifts, a
        # migration that already shipped starts answering a different event.
        catalog = self.catalog(REPO_ROOT)
        for index, family in enumerate(catalog["families"], start=1):
            self.assertEqual(family["event_kind"], 4096 + 30 * 256 + index,
                             f"{family['name']} is off the carved grid")
            self.assertEqual(family["id"], index)

    def test_reserved_family_kind_cannot_drift(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            self.reserved(catalog)["event_kind"] = 12000
            self.write(root, catalog)
            self.assertRule(root, "family-event-kind")
        finally:
            tmp.cleanup()

    def test_an_operation_cannot_be_declared_on_a_reserved_family(self) -> None:
        # A reserved family is a promise about numbering, not a served contract.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            operation = copy.deepcopy(catalog["operations"][0])
            operation["family"] = self.reserved(catalog)["name"]
            operation["name"] = "borrowed"
            catalog["operations"].append(operation)
            self.write(root, catalog)
            self.assertRule(root, "operation-inactive")
        finally:
            tmp.cleanup()

    def test_completeness_cannot_be_claimed_while_a_family_is_reserved(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            catalog["catalog_complete"] = True
            self.write(root, catalog)
            self.assertRule(root, "catalog-complete")
        finally:
            tmp.cleanup()

    def test_a_reserved_family_may_not_already_have_wire_constants(self) -> None:
        # Publishing a constant for a family nothing serves invites a caller to
        # speak an event with no listener.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            reserved = self.reserved(self.catalog(root))
            symbol = f"AIMEE_DB1_EVENT_{reserved['name'].upper()}"
            header = root / contract.HEADER
            header.write_text(
                header.read_text(encoding="utf-8").replace(
                    "#endif /* AIMEE_DB1_MODULE_API_H */",
                    f"#define {symbol} {reserved['event_kind']}u\n"
                    "#endif /* AIMEE_DB1_MODULE_API_H */"),
                encoding="utf-8")
            self.assertRule(root, "header-reserved")
        finally:
            tmp.cleanup()

    def test_header_drift_is_caught_in_every_direction(self) -> None:
        for original, replacement, rule in (
            ("AIMEE_DB1_EVENT_ECONOMIZER_STATE 11777u",
             "AIMEE_DB1_EVENT_ECONOMIZER_STATE 11778u", "header-event"),
            ("AIMEE_DB1_STAGE_ECONOMIZER_STATE 1u",
             "AIMEE_DB1_STAGE_ECONOMIZER_STATE 2u", "header-stage"),
            ("AIMEE_DB1_OP_STATE_SAVE 2u", "AIMEE_DB1_OP_STATE_SAVE 3u", "header-op"),
            ("AIMEE_DB1_STATUS_TOO_LONG 3u", "AIMEE_DB1_STATUS_TOO_LONG 9u", "header-status"),
        ):
            with self.subTest(rule=rule):
                tmp = sandbox()
                try:
                    root = Path(tmp.name)
                    header = root / contract.HEADER
                    text = header.read_text(encoding="utf-8")
                    self.assertIn(original, text)
                    header.write_text(text.replace(original, replacement), encoding="utf-8")
                    self.assertRule(root, rule)
                finally:
                    tmp.cleanup()

    def test_active_families_and_declared_stages_must_agree(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            path = root / contract.PROCESS_CONTRACTS
            document = json.loads(path.read_text(encoding="utf-8"))
            for component in document["components"]:
                if component["id"] == "db1":
                    component["stages"][0]["event_kind"] = 11999
            path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            self.assertRule(root, "stage-binding")
        finally:
            tmp.cleanup()

    def test_a_stage_without_an_active_family_is_refused(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            catalog["families"][0]["name"] = "renamed_state"
            catalog["operations"][0]["family"] = "renamed_state"
            catalog["operations"][1]["family"] = "renamed_state"
            self.write(root, catalog)
            # The header still names the old family, so drift is caught first.
            with self.assertRaises(contract.ContractError):
                contract.run(root)
        finally:
            tmp.cleanup()

    def test_every_operation_is_keyed(self) -> None:
        # DB1 rows belong to a conversation or session; an unkeyed read would
        # cross that boundary.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            catalog["operations"][0]["request"]["fields"] = ["state"]
            self.write(root, catalog)
            self.assertRule(root, "request-key")
        finally:
            tmp.cleanup()

    def test_a_reply_that_carries_nothing_declares_no_budget(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            catalog["operations"][1]["reply"]["max_bytes"] = 64
            self.write(root, catalog)
            self.assertRule(root, "reply-bytes")
        finally:
            tmp.cleanup()

    def test_unknown_and_duplicate_keys_fail_closed(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            catalog["unexpected"] = True
            self.write(root, catalog)
            self.assertRule(root, "keys")

            (root / contract.CATALOG).write_text(
                '{"module": "db1", "module": "db1"}', encoding="utf-8")
            self.assertRule(root, "duplicate-key")
        finally:
            tmp.cleanup()

    def test_covers_documents_the_remaining_migration(self) -> None:
        # The point of a reservation is that the outstanding work is countable
        # from the catalog rather than rediscovered.
        catalog = self.catalog(REPO_ROOT)
        for family in catalog["families"]:
            self.assertTrue(family["covers"].strip(),
                            f"{family['name']} reserves a kind but names no sources")


if __name__ == "__main__":
    unittest.main(verbosity=2)
