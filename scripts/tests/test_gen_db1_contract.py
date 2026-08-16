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
    """A copy of the files the contract binds together.

    The DB1 sources are recreated as empty files rather than copied: the
    retired-sources rule only asks which names exist and which the Makefile
    still mentions, so their contents are irrelevant and copying sixty files
    per test is not.
    """
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    for relative in (contract.CATALOG, contract.HEADER, contract.PROCESS_CONTRACTS,
                     contract.MAKEFILE):
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO_ROOT / relative, target)
    for source in (REPO_ROOT / contract.SOURCE_DIR).glob("*.c"):
        (root / contract.SOURCE_DIR / source.name).touch()
    # The generated clients are real content, not placeholders: the contract
    # compares them byte for byte.
    for client in (REPO_ROOT / contract.CLIENT_DIR).glob("*.c"):
        target = root / contract.CLIENT_DIR / client.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(client, target)
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
            # Subsumed by the whole-file check: a constant for a family nothing
            # serves is simply not what the catalog generates.
            self.assertRule(root, "header-stale")
        finally:
            tmp.cleanup()

    def test_header_drift_is_caught_in_every_direction(self) -> None:
        for original, replacement, rule in (
            ("AIMEE_DB1_EVENT_ECONOMIZER_STATE 11777u",
             "AIMEE_DB1_EVENT_ECONOMIZER_STATE 11778u", "header-stale"),
            ("AIMEE_DB1_STAGE_ECONOMIZER_STATE 1u",
             "AIMEE_DB1_STAGE_ECONOMIZER_STATE 2u", "header-stale"),
            ("AIMEE_DB1_OP_STATE_SAVE 2u", "AIMEE_DB1_OP_STATE_SAVE 3u", "header-stale"),
            ("AIMEE_DB1_STATUS_TOO_LONG 3u", "AIMEE_DB1_STATUS_TOO_LONG 9u", "header-stale"),
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

    def test_a_family_cannot_retire_a_source_the_daemon_still_links(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            active = next(f for f in catalog["families"] if f["active"])
            active["retired_sources"] = sorted(set(active["retired_sources"]) | {"db1_init.c"})
            self.write(root, catalog)
            self.assertRule(root, "retired-still-linked")
        finally:
            tmp.cleanup()

    def test_a_source_cannot_leave_the_daemon_unclaimed(self) -> None:
        # The half that catches a migration in progress: dropping a domain from
        # the daemon's link without a family owning it would leave nothing
        # saying where its callers went.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            makefile = root / contract.MAKEFILE
            text = makefile.read_text(encoding="utf-8")
            self.assertIn("modules/db1/checkpoints.c", text)
            makefile.write_text(text.replace(" modules/db1/checkpoints.c", "", 1),
                                encoding="utf-8")
            self.assertRule(root, "retired-unclaimed")
        finally:
            tmp.cleanup()

    def test_a_reserved_family_cannot_retire_anything(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            self.reserved(catalog)["retired_sources"] = ["git_ownership.c"]
            self.write(root, catalog)
            self.assertRule(root, "retired-reserved")
        finally:
            tmp.cleanup()

    def test_two_families_cannot_retire_the_same_source(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            actives = [f for f in catalog["families"] if f["active"]]
            self.assertGreaterEqual(len(actives), 2)
            actives[0]["retired_sources"] = ["git_ownership.c"]
            actives[1]["retired_sources"] = ["git_ownership.c"]
            self.write(root, catalog)
            self.assertRule(root, "retired-duplicate")
        finally:
            tmp.cleanup()

    def test_the_module_adapter_is_never_claimed(self) -> None:
        # It is served by the module process alone and was never in the daemon,
        # so it is not evidence of a migration and must not need claiming.
        catalog = self.catalog(REPO_ROOT)
        for family in catalog["families"]:
            self.assertNotIn("module_adapter.c", family["retired_sources"])
        contract.run(REPO_ROOT)

    def test_generation_is_deterministic(self) -> None:
        catalog = contract.validate_catalog(contract.load_json(REPO_ROOT / contract.CATALOG))
        self.assertEqual(contract.header_bytes(catalog), contract.header_bytes(catalog))

    def test_writing_the_header_is_idempotent(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            contract.run(root, write=True)
            once = (root / contract.HEADER).read_text(encoding="utf-8")
            contract.run(root, write=True)
            self.assertEqual(once, (root / contract.HEADER).read_text(encoding="utf-8"))
        finally:
            tmp.cleanup()

    def test_a_new_family_brings_its_constants_and_widens_the_bounds(self) -> None:
        # The reason to generate at all: adding a family should not require
        # remembering that the decoder's fixed array is sized by the widest
        # request in the catalog. Getting that wrong was a stack overflow once.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            reserved = self.reserved(catalog)
            reserved["active"] = True
            catalog["operations"].append({
                "family": reserved["name"], "id": 1, "name": "probe_touch",
                "wire_format": "db1-fields-v1", "scope": "session",
                "transaction": "single", "idempotency": "idempotent",
                "results": ["ok", "invalid", "failed"],
                "request": {"fields": ["key", "a", "b", "c"]},
                "reply": {"payload": "none", "max_bytes": 0},
            })
            self.write(root, catalog)
            # Activating a family without declaring its stage is refused, which
            # is the cross-check doing its job -- so declare it.
            path = root / contract.PROCESS_CONTRACTS
            document = json.loads(path.read_text(encoding="utf-8"))
            for component in document["components"]:
                if component["id"] == "db1":
                    component["stages"].append({
                        "id": reserved["id"],
                        "name": "db1-" + reserved["name"].replace("_", "-"),
                        "event_kind": reserved["event_kind"],
                    })
            path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")

            contract.run(root, write=True)
            header = (root / contract.HEADER).read_text(encoding="utf-8")
            upper = reserved["name"].upper()
            self.assertIn(f"#define AIMEE_DB1_EVENT_{upper} {reserved['event_kind']}u", header)
            self.assertIn("AIMEE_DB1_OP_PROBE_TOUCH", header)
            self.assertRegex(header, r"AIMEE_DB1_FIELDS_MAX\s+4u")
        finally:
            tmp.cleanup()

    def test_a_reserved_family_emits_nothing(self) -> None:
        catalog = contract.validate_catalog(contract.load_json(REPO_ROOT / contract.CATALOG))
        header = contract.header_bytes(catalog)
        families = catalog["families"]
        for name, family in families.items():
            if not family["active"]:
                self.assertNotIn(name.upper(), header,
                                 f"reserved family {name} leaked into the wire header")

    def test_every_db1_source_is_claimed_exactly_once(self) -> None:
        # The property that makes the catalog a map: an unclaimed domain is one
        # nobody is planning to move, and a twice-claimed one is two families
        # expecting to own the same rows.
        catalog = self.catalog(REPO_ROOT)
        owner: dict[str, str] = {}
        for family in catalog["families"]:
            for source in family["sources"]:
                self.assertNotIn(source, owner, f"{source} claimed twice")
                owner[source] = family["name"]
        for source in catalog["infrastructure_sources"]:
            self.assertNotIn(source, owner, f"{source} is infrastructure and claimed")
            owner[source] = "(infrastructure)"
        on_disk = {path.stem for path in (REPO_ROOT / contract.SOURCE_DIR).glob("*.c")}
        self.assertEqual(set(owner), on_disk)

    def test_an_unclaimed_or_duplicated_source_is_refused(self) -> None:
        for mutate, rule in (
            (lambda c: c["families"][2]["sources"].pop(), "source-unclaimed"),
            (lambda c: c["families"][2]["sources"].append(c["families"][3]["sources"][0]),
             "source-duplicate"),
            (lambda c: c["infrastructure_sources"].append("ghost_domain"), "source-absent"),
        ):
            with self.subTest(rule=rule):
                tmp = sandbox()
                try:
                    root = Path(tmp.name)
                    catalog = self.catalog(root)
                    mutate(catalog)
                    catalog["families"][2]["sources"].sort()
                    catalog["infrastructure_sources"].sort()
                    self.write(root, catalog)
                    self.assertRule(root, rule)
                finally:
                    tmp.cleanup()

    def test_a_coupled_ledger_cannot_be_split_across_families(self) -> None:
        # This one is not hygiene. server_compute.c records that separating the
        # reservation ledger from the launch left paid-for jobs nothing could
        # replay, and a family is the unit that activates -- so two halves in
        # two families is a plan to separate them again.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            group = catalog["coupled_sources"][0]["sources"]
            holder = next(f for f in catalog["families"] if group[0] in f["sources"])
            other = next(f for f in catalog["families"] if f["name"] != holder["name"])
            holder["sources"] = sorted(s for s in holder["sources"] if s != group[0])
            other["sources"] = sorted(other["sources"] + [group[0]])
            self.write(root, catalog)
            self.assertRule(root, "coupled-split")
        finally:
            tmp.cleanup()

    def test_the_shipped_coupling_names_a_reason(self) -> None:
        catalog = self.catalog(REPO_ROOT)
        self.assertTrue(catalog["coupled_sources"])
        for group in catalog["coupled_sources"]:
            self.assertGreaterEqual(len(group["sources"]), 2)
            self.assertTrue(group["reason"].strip(),
                            "a coupling with no reason cannot be reviewed")

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
