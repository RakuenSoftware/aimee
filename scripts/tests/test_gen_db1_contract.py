#!/usr/bin/env python3
"""Failure modes of the DB1 operation-catalog contract."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import re
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
    # The headers are real content, not placeholders: the generator reads them
    # to find which one declares each operation, so an empty file changes what
    # it emits.
    for header in (REPO_ROOT / contract.SOURCE_DIR).glob("*.h"):
        shutil.copy2(header, root / contract.SOURCE_DIR / header.name)
    # The generated clients are real content, not placeholders: the contract
    # compares them byte for byte.
    for client in (REPO_ROOT / contract.CLIENT_DIR).glob("*.c"):
        target = root / contract.CLIENT_DIR / client.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(client, target)
    # The generated stage handlers and their declarations are compared byte for
    # byte too, so they are copied rather than touched.
    for generated in list((REPO_ROOT / contract.SOURCE_DIR).glob("*_stage.c")) + \
            [REPO_ROOT / contract.STAGES_HEADER]:
        shutil.copy2(generated, root / contract.SOURCE_DIR / generated.name)
    # The adapter is real content too: it is where the dispatch rule reads which
    # stages are actually routed, and an empty file reads as none of them.
    shutil.copy2(REPO_ROOT / contract.SOURCE_DIR / "module_adapter.c",
                 root / contract.SOURCE_DIR / "module_adapter.c")
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

    def route(self, root: Path, family: dict) -> None:
        """Route a newly activated family's stage in the sandbox adapter.

        Activating a family in the catalog is only half of it: the adapter's
        switch is what makes the stage reachable, and the dispatch rule holds
        the two together. These tests activate a family to exercise something
        else, so they have to do the other half too.
        """
        adapter = root / contract.SOURCE_DIR / "module_adapter.c"
        label = f"case AIMEE_DB1_STAGE_{family['name'].upper()}:"
        adapter.write_text(
            adapter.read_text(encoding="utf-8").replace(
                "   default:", f"   {label}\n      break;\n   default:", 1),
            encoding="utf-8")

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
            # Reserve one, because the real catalog no longer has any: every
            # family is served. A test that waited for the repo to supply the
            # state it is testing stops testing anything the day the migration
            # finishes -- which is the day this one broke.
            catalog["catalog_complete"] = True
            # A fresh reserved family rather than deactivating a served one:
            # every family now retires sources, and un-serving one of those
            # trips retired-reserved before completeness is ever considered.
            reserved = dict(catalog["families"][0])
            reserved.update({"id": len(catalog["families"]) + 1, "name": "zz_reserved",
                             "event_kind": 11776 + len(catalog["families"]) + 1,
                             "active": False, "covers": "secrets", "sources": ["secrets"],
                             "retired_sources": []})
            catalog["families"].append(reserved)
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
            # Put a source back in the daemon's link and un-retire it, rather
            # than naming one that happens to still be there: every family is
            # served now, so there is no longer such a source, and every earlier
            # version of this test named one that later migrated -- first
            # checkpoints.c, then wfe_store.c. The state under test is built
            # here so it cannot expire again.
            catalog = self.catalog(root)
            family = next(f for f in catalog["families"] if f["retired_sources"])
            retired = family["retired_sources"][0]
            family["retired_sources"] = [s for s in family["retired_sources"] if s != retired]
            self.write(root, catalog)
            makefile = root / contract.MAKEFILE
            text = makefile.read_text(encoding="utf-8")
            self.assertNotIn(f"modules/db1/{retired}", text)
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
        catalog = contract.validate_catalog(contract.load_json(REPO_ROOT / contract.CATALOG),
                                            REPO_ROOT)
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
            self.route(root, reserved)
            catalog["operations"].append({
                "family": reserved["name"], "id": 1, "name": "probe_touch",
                "wire_format": "db1-fields-v2", "scope": "session",
                "transaction": "single", "idempotency": "idempotent",
                "results": ["ok", "invalid", "failed"],
                "request": {"fields": [{"name": n, "type": "text", "required": True}
                                       for n in ("key", "a", "b", "c")]},
                "reply": {"fields": [], "max_bytes": 0},
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
            widest = int(re.search(r"AIMEE_DB1_FIELDS_MAX\s+(\d+)u", header).group(1))
            self.assertGreaterEqual(widest, 4, "the four-field operation must fit")
        finally:
            tmp.cleanup()

    def test_a_reserved_family_emits_nothing(self) -> None:
        catalog = contract.validate_catalog(contract.load_json(REPO_ROOT / contract.CATALOG),
                                            REPO_ROOT)
        header = contract.header_bytes(catalog)
        families = catalog["families"]
        for name, family in families.items():
            if family["active"]:
                continue
            # The family's own constants, not its bare name. A reserved family
            # called "delegation" shares a substring with the perfectly legal
            # AIMEE_DB1_OP_AGENT_LOG_DELEGATION_PATTERNS, and matching the name
            # made an ordinary operation look like a leak.
            for constant in (f"AIMEE_DB1_EVENT_{name.upper()}",
                             f"AIMEE_DB1_STAGE_{name.upper()}"):
                self.assertNotIn(constant, header,
                                 f"reserved family {name} leaked {constant} into the wire header")

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
        # Generated stage handlers are wire, not domains, so no family claims
        # them -- the same exemption the validator applies.
        generated = {f"{family['name']}_stage" for family in catalog["families"]
                     if any(o.get("c_name") and o["family"] == family["name"]
                            for o in catalog["operations"])}
        self.assertEqual(set(owner), on_disk - generated)

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

    def integer_catalog(self, root: Path) -> dict:
        """Activate a reserved family with one text-and-integer operation."""
        catalog = self.catalog(root)
        reserved = self.reserved(catalog)
        reserved["active"] = True
        self.route(root, reserved)
        catalog["operations"].append({
            "family": reserved["name"], "id": 1, "name": "probe_forget_if_job",
            "c_name": "db1_probe_forget_if_job", "c_params": ["probe_id", "job_id"],
            "wire_format": "db1-fields-v2", "scope": "session", "transaction": "single",
            "idempotency": "idempotent", "results": ["ok", "invalid", "failed"],
            "request": {"fields": [{"name": "key", "type": "text", "required": True},
                                   {"name": "job", "type": "int", "required": True}]},
            "reply": {"fields": [], "max_bytes": 0},
        })
        self.write(root, catalog)
        path = root / contract.PROCESS_CONTRACTS
        document = json.loads(path.read_text(encoding="utf-8"))
        for component in document["components"]:
            if component["id"] == "db1":
                component["stages"].append({
                    "id": reserved["id"],
                    "name": "db1-" + reserved["name"].replace("_", "-"),
                    "event_kind": reserved["event_kind"]})
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        return catalog

    def test_an_integer_argument_is_converted_on_both_sides(self) -> None:
        # Integers travel as decimal text, so the client prints and the stage
        # parses. Half of that is a silent corruption: a client that sent an int
        # raw would be read as whatever those bytes spell.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.integer_catalog(root)
            contract.run(root, write=True)
            reserved = next(f for f in catalog["families"]
                            if any(o["family"] == f["name"] and o["name"] == "probe_forget_if_job"
                                   for o in catalog["operations"]))
            client = (root / contract.CLIENT_DIR / f"{reserved['name']}.c").read_text()
            stage = (root / contract.SOURCE_DIR / f"{reserved['name']}_stage.c").read_text()

            self.assertIn("int db1_probe_forget_if_job(const char *probe_id, int job_id)", client)
            self.assertIn('snprintf(arg1, sizeof arg1, "%d", job_id);', client)
            self.assertIn("{probe_id, arg1}", client)
            # An int has no null and no empty case, so only the text argument
            # is checked -- and it is checked for both.
            self.assertIn("if (!probe_id || !probe_id[0])", client)
            self.assertNotIn("!job_id", client)

            self.assertIn("parse_int(field[1], &parsed1)", stage)
            self.assertIn("rc = db1_probe_forget_if_job(field[0], parsed1);", stage)
        finally:
            tmp.cleanup()

    def test_a_partial_integer_is_refused_rather_than_truncated(self) -> None:
        # "12abc" must not become 12: the module would act on a value the caller
        # never sent. The generated parser checks the whole field.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.integer_catalog(root)
            contract.run(root, write=True)
            # The stage this test generated, named rather than globbed. A glob
            # yields whatever readdir does, so "the first *_stage.c" was really
            # "whichever the filesystem lists first" -- it passed for as long as
            # every family happened to take an int somewhere, then found a
            # family whose arguments are all int64 and read a file with no
            # parse_int in it at all.
            reserved = next(f for f in catalog["families"]
                            if any(o["family"] == f["name"] and o["name"] == "probe_forget_if_job"
                                   for o in catalog["operations"]))
            stage = root / contract.SOURCE_DIR / f"{reserved['name']}_stage.c"
            text = stage.read_text(encoding="utf-8")
            self.assertIn("*end != '\\0'", text)
            self.assertIn("errno != 0", text)
            self.assertIn("value < INT_MIN || value > INT_MAX", text)
        finally:
            tmp.cleanup()

    def test_field_types_are_closed(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            # "blob" rather than "float": float joined the set when clarify's
            # score needed it, and a test that pins the set has to name
            # something still outside it or it stops testing anything.
            catalog["operations"][0]["request"]["fields"][0]["type"] = "blob"
            self.write(root, catalog)
            self.assertRule(root, "field-type")
        finally:
            tmp.cleanup()

    def optional_catalog(self, root: Path) -> dict:
        """Activate a reserved family with one required/optional operation."""
        catalog = self.catalog(root)
        reserved = self.reserved(catalog)
        reserved["active"] = True
        self.route(root, reserved)
        catalog["operations"].append({
            "family": reserved["name"], "id": 1, "name": "probe_status_set",
            "c_name": "db1_probe_status_set",
            "c_params": ["probe_id", "status", "pipeline_id", "error"],
            "wire_format": "db1-fields-v2", "scope": "session", "transaction": "single",
            "idempotency": "idempotent", "results": ["ok", "invalid", "failed"],
            "request": {"fields": [
                {"name": "key", "type": "text", "required": True},
                {"name": "status", "type": "text", "required": True},
                {"name": "pipeline", "type": "text", "required": False},
                {"name": "error", "type": "text", "required": False}]},
            "reply": {"fields": [], "max_bytes": 0},
        })
        self.write(root, catalog)
        path = root / contract.PROCESS_CONTRACTS
        document = json.loads(path.read_text(encoding="utf-8"))
        for component in document["components"]:
            if component["id"] == "db1":
                component["stages"].append({
                    "id": reserved["id"],
                    "name": "db1-" + reserved["name"].replace("_", "-"),
                    "event_kind": reserved["event_kind"]})
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        return reserved

    def test_an_optional_field_is_carried_and_only_required_ones_guarded(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            reserved = self.optional_catalog(root)
            contract.run(root, write=True)
            client = (root / contract.CLIENT_DIR / f"{reserved['name']}.c").read_text()
            stage = (root / contract.SOURCE_DIR / f"{reserved['name']}_stage.c").read_text()

            # Only the required arguments are refused locally.
            self.assertIn("if (!probe_id || !probe_id[0] || !status || !status[0])", client)
            self.assertNotIn("!pipeline_id[0]", client)
            # An absent optional value travels as empty, which is how the
            # domains already read NULL.
            self.assertIn('pipeline_id ? pipeline_id : ""', client)
            self.assertIn('error ? error : ""', client)
            # The stage checks the required fields and lets the rest be blank.
            self.assertIn("if (!field[0][0])", stage)
            self.assertIn("if (!field[1][0])", stage)
            self.assertNotIn("if (!field[2][0])", stage)
        finally:
            tmp.cleanup()

    def test_arity_is_checked_before_any_field_is_read(self) -> None:
        # Only `count` entries of field[] are initialised. Testing field[2] on a
        # two-field frame reads uninitialised memory, so the arity check has to
        # come first -- it did not, briefly, and this is what says so.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            reserved = self.optional_catalog(root)
            contract.run(root, write=True)
            stage = (root / contract.SOURCE_DIR / f"{reserved['name']}_stage.c").read_text()
            case = stage[stage.index("case AIMEE_DB1_OP_PROBE_STATUS_SET"):]
            case = case[:case.index("break;")]
            self.assertLess(case.index("if (count != 4u)"), case.index("if (!field[0][0])"),
                            "a field was read before the arity check")
        finally:
            tmp.cleanup()

    def test_a_scoped_operation_cannot_make_its_key_optional(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            catalog["operations"][0]["request"]["fields"][0]["required"] = False
            self.write(root, catalog)
            self.assertRule(root, "field-required")
        finally:
            tmp.cleanup()

    def test_a_reply_declares_its_fields_and_the_frame_counts_them(self) -> None:
        # The reply counts for the same reason the request does: an operation
        # answering with a row, or a list of them, has somewhere to put the
        # values. A write sends count 0; a single value sends count 1.
        catalog = self.catalog(REPO_ROOT)
        for operation in catalog["operations"]:
            reply = operation["reply"]
            self.assertIn("fields", reply, f"{operation['name']} reply is not counted")
            self.assertEqual(bool(reply["fields"]), reply["max_bytes"] > 0)
        stage = (REPO_ROOT / contract.SOURCE_DIR / "git_ownership_stage.c").read_text()
        client = (REPO_ROOT / contract.CLIENT_DIR / "git_ownership.c").read_text()
        # Both sides speak count, not a single length.
        # Both sides speak a count, not a single length.
        self.assertIn("aimee_db1_put_u32(out + 4u, count);", stage)
        self.assertIn("uint32_t fields_in = aimee_db1_get_u32(response + 4u);", client)

    def test_every_operation_is_keyed(self) -> None:
        # DB1 rows belong to a conversation or session; an unkeyed read would
        # cross that boundary.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            catalog["operations"][0]["request"]["fields"] = [
                {"name": "state", "type": "text", "required": True}]
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


    def test_an_active_family_must_have_its_stage_dispatched(self) -> None:
        # A generated stage that nothing routes to compiles, links and passes
        # its own tests while being unreachable. That is what happened to the
        # conversation family: the adapter's switch is hand-written, because the
        # first family answers a different wire format, and nothing tied the two
        # together until this rule.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            adapter = root / contract.SOURCE_DIR / "module_adapter.c"
            adapter.write_text(
                adapter.read_text(encoding="utf-8").replace(
                    "case AIMEE_DB1_STAGE_CONVERSATION:", "case 999999:"),
                encoding="utf-8")
            self.assertRule(root, "stage-undispatched")
        finally:
            tmp.cleanup()

    def listing(self, catalog: dict) -> dict:
        for operation in catalog["operations"]:
            if "list" in operation["reply"]:
                return operation
        self.skipTest("no list operation to exercise")

    def test_a_list_names_the_parameter_that_receives_the_rows(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            self.listing(catalog)["reply"]["list"]["out"] = "not_a_parameter"
            self.write(root, catalog)
            self.assertRule(root, "reply-list")
        finally:
            tmp.cleanup()

    def test_a_list_bound_must_be_an_integer_field(self) -> None:
        # The bound is the allocation on both sides. Pointing it at a text field
        # would make the stage size an array from a string.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            operation = self.listing(catalog)
            operation["reply"]["list"]["bound"] = operation["c_params"][0]
            self.write(root, catalog)
            self.assertRule(root, "reply-list")
        finally:
            tmp.cleanup()

    def test_a_list_ceiling_is_bounded(self) -> None:
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            self.listing(catalog)["reply"]["list"]["max_rows"] = 1 << 20
            self.write(root, catalog)
            self.assertRule(root, "integer")
        finally:
            tmp.cleanup()

    def test_a_list_must_declare_the_row_it_repeats(self) -> None:
        # A list is a struct repeated. Without the struct there is no row type
        # to write the members back into.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            del self.listing(catalog)["reply"]["struct"]
            self.write(root, catalog)
            self.assertRule(root, "reply-list")
        finally:
            tmp.cleanup()

    def test_the_generated_list_client_divides_the_reply_by_the_row_width(self) -> None:
        # The width is never sent: an operation knows how wide its rows are, so
        # the reply's own value count is what carries the length.
        client = (REPO_ROOT / contract.CLIENT_DIR / "conversation.c").read_text(encoding="utf-8")
        self.assertIn("int wire_rows = (int)(wire_filled / 8u);", client)
        self.assertIn("wire_filled % 8u != 0u", client)

    def test_the_generated_list_stage_bounds_what_it_allocates(self) -> None:
        stage = (REPO_ROOT / contract.SOURCE_DIR / "conversation_stage.c").read_text(
            encoding="utf-8")
        self.assertIn("if (parsed2 <= 0 || parsed2 > 64)", stage)
        self.assertIn("calloc((size_t)parsed2, sizeof *found)", stage)


    def double_catalog(self, root: Path) -> dict:
        """Activate a reserved family with one row carrying a double member."""
        catalog = self.catalog(root)
        reserved = self.reserved(catalog)
        reserved["active"] = True
        self.route(root, reserved)
        catalog["operations"].append({
            "family": reserved["name"], "id": 1, "name": "probe_rate_get",
            "c_name": "db1_probe_rate_get", "c_params": ["probe_id", "out"],
            "wire_format": "db1-fields-v2", "scope": "session", "transaction": "none",
            "idempotency": "safe", "results": ["ok", "missing", "invalid", "failed"],
            "request": {"fields": [{"name": "key", "type": "text", "required": True}]},
            "reply": {"struct": "db1_probe_rate_t",
                      "fields": [{"name": "label", "type": "text"},
                                 {"name": "rate", "type": "double"}],
                      "max_bytes": 256}})
        self.write(root, catalog)
        path = root / contract.PROCESS_CONTRACTS
        document = json.loads(path.read_text(encoding="utf-8"))
        for component in document["components"]:
            if component["id"] == "db1":
                component["stages"].append({
                    "id": reserved["id"],
                    "name": "db1-" + reserved["name"].replace("_", "-"),
                    "event_kind": reserved["event_kind"]})
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        return catalog

    def test_a_double_member_round_trips_through_text(self) -> None:
        # A cost or a rate crosses as decimal text like every other number. The
        # spec has to be %.17g: %g rounds to six significant digits, so a value
        # would arrive as a DIFFERENT number while looking entirely plausible.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.double_catalog(root)
            contract.run(root, write=True)
            reserved = next(f for f in catalog["families"]
                            if any(o["family"] == f["name"] and o["name"] == "probe_rate_get"
                                   for o in catalog["operations"]))
            client = (root / contract.CLIENT_DIR / f"{reserved['name']}.c").read_text()
            stage = (root / contract.SOURCE_DIR / f"{reserved['name']}_stage.c").read_text()

            self.assertIn('"%.17g"', stage)
            # strtod takes two arguments. Pairing it with strtol's base is the
            # mistake this pins: it compiles nowhere but was generated once.
            self.assertIn("strtod(slot1, NULL)", client)
            self.assertNotIn("strtod(slot1, NULL, 10)", client)
        finally:
            tmp.cleanup()

    def test_a_numeric_slot_has_room_for_the_widest_double(self) -> None:
        # "-1.2345678901234567e-308" is 24 characters, so a 24-byte slot has no
        # room for the terminator and the value arrives truncated -- as a
        # different, still-parseable number.
        widest = len("-1.2345678901234567e-308") + 1
        self.assertGreaterEqual(contract.NUMERIC_TEXT, widest)
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            self.double_catalog(root)
            contract.run(root, write=True)
            # A DECLARATION of width 24, not the characters "[24]": a family
            # with two dozen fields indexes field[24] in the ordinary course of
            # things, and matching that says nothing about slot widths.
            declared = re.compile(r"char\s+\(?\*?\w+\)?\s*\[\s*24\s*\]")
            for path in list((root / contract.SOURCE_DIR).glob("*_stage.c")) + \
                    list((root / contract.CLIENT_DIR).glob("*.c")):
                found = declared.search(path.read_text())
                self.assertIsNone(found,
                                  f"{path.name} still sizes a numeric slot at 24")
        finally:
            tmp.cleanup()

    def shaped_catalog(self, root: Path) -> dict:
        """A family carrying the three shapes coord_jobs needed to migrate."""
        catalog = self.catalog(root)
        reserved = self.reserved(catalog)
        reserved["active"] = True
        self.route(root, reserved)
        catalog["operations"] += [
            {"family": reserved["name"], "id": 1, "name": "probe_claim",
             "c_name": "db1_probe_claim", "c_params": ["probe_id", "out"],
             "c_returns": "member", "c_member": "id",
             "wire_format": "db1-fields-v2", "scope": "global", "transaction": "single",
             "idempotency": "unsafe", "results": ["ok", "missing", "failed"],
             "request": {"fields": [{"name": "probe_id", "type": "int", "required": True}]},
             "reply": {"struct": "db1_probe_t",
                       "fields": [{"name": "id", "type": "int"},
                                  {"name": "label", "type": "text"}],
                       "max_bytes": 256}},
            {"family": reserved["name"], "id": 2, "name": "probe_dispatch",
             "c_name": "db1_probe_dispatch",
             "c_params": ["probe_id", "role_out", "role_cap", "note_out", "note_cap"],
             "wire_format": "db1-fields-v2", "scope": "global", "transaction": "none",
             "idempotency": "safe", "results": ["ok", "invalid", "failed"],
             "request": {"fields": [{"name": "probe_id", "type": "int", "required": True}]},
             "reply": {"scalars": True,
                       "fields": [{"name": "role", "type": "text", "width": "DB1_PROBE_ROLE_LEN"},
                                  {"name": "note", "type": "text", "width": "DB1_PROBE_NOTE_LEN"}],
                       "max_bytes": 1024}},
            {"family": reserved["name"], "id": 3, "name": "probe_conflicts",
             "c_name": "db1_probe_conflicts", "c_params": ["probe_id"],
             "c_returns": "found",
             "wire_format": "db1-fields-v2", "scope": "global", "transaction": "none",
             "idempotency": "safe", "results": ["ok", "missing", "failed"],
             "request": {"fields": [{"name": "probe_id", "type": "int", "required": True}]},
             "reply": {"fields": [], "max_bytes": 0}}]
        self.write(root, catalog)
        path = root / contract.PROCESS_CONTRACTS
        document = json.loads(path.read_text(encoding="utf-8"))
        for component in document["components"]:
            if component["id"] == "db1":
                component["stages"].append({
                    "id": reserved["id"],
                    "name": "db1-" + reserved["name"].replace("_", "-"),
                    "event_kind": reserved["event_kind"]})
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        return reserved["name"]

    def test_a_returned_member_is_the_rows_own_value(self) -> None:
        # A claim hands back the row AND its id. The id is not a status, so the
        # stage must not read a positive return as a failure -- which is what
        # the row-shaped default does, and it swallowed every claim until the
        # end-to-end fixture caught it.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            family = self.shaped_catalog(root)
            contract.run(root, write=True)
            client = (root / contract.CLIENT_DIR / f"{family}.c").read_text()
            stage = (root / contract.SOURCE_DIR / f"{family}_stage.c").read_text()
            self.assertIn("return out->id;", client)
            claim = stage[stage.index("PROBE_CLAIM:"):]
            claim = claim[:claim.index("break;")]
            self.assertIn("found = 1;", claim)
        finally:
            tmp.cleanup()

    def test_a_text_scalar_outlives_the_case_that_produced_it(self) -> None:
        # The reply is written after the switch closes, so a buffer declared in
        # the case block is read after its scope ends. One allocation per call,
        # freed once, sized by the widths the contract declares.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            family = self.shaped_catalog(root)
            contract.run(root, write=True)
            stage = (root / contract.SOURCE_DIR / f"{family}_stage.c").read_text()
            self.assertIn("calloc(1u, DB1_PROBE_ROLE_LEN + DB1_PROBE_NOTE_LEN)", stage)
            self.assertIn("char *scalar1 = scalar_owned + DB1_PROBE_ROLE_LEN;", stage)
            self.assertIn("free(scalar_owned);", stage)
            self.assertNotIn("char scalar0[DB1_PROBE_ROLE_LEN];", stage)
            client = (root / contract.CLIENT_DIR / f"{family}.c").read_text()
            self.assertIn("char *const values[] = {role_out, note_out};", client)
            self.assertIn("const size_t caps[] = {role_cap, note_cap};", client)
        finally:
            tmp.cleanup()

    def test_a_text_scalar_must_declare_how_wide_it_is(self) -> None:
        # The stage cannot see the caller's buffer, so silence here would mean
        # picking a width -- and picking one silently truncates.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            self.shaped_catalog(root)
            catalog = json.loads((root / contract.CATALOG).read_text(encoding="utf-8"))
            for operation in catalog["operations"]:
                if operation["name"] == "probe_dispatch":
                    del operation["reply"]["fields"][0]["width"]
            self.write(root, catalog)
            with self.assertRaises(contract.ContractError):
                contract.run(root, write=False)
        finally:
            tmp.cleanup()

    def test_a_yes_no_answer_keeps_all_three_answers(self) -> None:
        # Nothing comes back but the status. Folding "no" into "failed" would
        # report an empty result as an outage; forgetting the stage's flag
        # answers yes to every question, which is worse.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            family = self.shaped_catalog(root)
            contract.run(root, write=True)
            client = (root / contract.CLIENT_DIR / f"{family}.c").read_text()
            body = client[client.index("int db1_probe_conflicts("):]
            body = body[:body.index("\n}")]
            self.assertIn("AIMEE_DB1_STATUS_MISSING", body)
            self.assertIn("return 0;", body)
            self.assertIn("? 1 : -1;", body)
            stage = (root / contract.SOURCE_DIR / f"{family}_stage.c").read_text()
            case = stage[stage.index("PROBE_CONFLICTS:"):]
            case = case[:case.index("break;")]
            self.assertIn("found = 1;", case)
        finally:
            tmp.cleanup()

    def array_catalog(self, root: Path) -> str:
        """A family whose row carries `char member[N][W]`."""
        catalog = self.catalog(root)
        reserved = self.reserved(catalog)
        reserved["active"] = True
        self.route(root, reserved)
        row = [{"name": "id", "type": "text"},
               {"name": "tags", "type": "text", "repeat": 4},
               {"name": "tag_count", "type": "int"}]
        catalog["operations"].append({
            "family": reserved["name"], "id": 1, "name": "probe_tagged_get",
            "c_name": "db1_probe_tagged_get", "c_params": ["probe_id", "out"],
            "wire_format": "db1-fields-v2", "scope": "global", "transaction": "none",
            "idempotency": "safe", "results": ["ok", "invalid", "failed"],
            "request": {"fields": [{"name": "probe_id", "type": "text", "required": True}]},
            "reply": {"struct": "db1_probe_tagged_t", "out": "out",
                      "fields": row, "max_bytes": 1024}})
        self.write(root, catalog)
        path = root / contract.PROCESS_CONTRACTS
        document = json.loads(path.read_text(encoding="utf-8"))
        for component in document["components"]:
            if component["id"] == "db1":
                component["stages"].append({
                    "id": reserved["id"],
                    "name": "db1-" + reserved["name"].replace("_", "-"),
                    "event_kind": reserved["event_kind"]})
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        return reserved["name"]

    def test_an_array_member_crosses_as_its_elements(self) -> None:
        # A struct member does not have variable arity: the array is always N
        # wide and an unused slot is an empty string, not an absent field. The
        # element spelling is what makes this the whole capability -- C already
        # writes `out->tags[2]`, so every emitter carries it unchanged.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            family = self.array_catalog(root)
            contract.run(root, write=True)
            client = (root / contract.CLIENT_DIR / f"{family}.c").read_text()
            stage = (root / contract.SOURCE_DIR / f"{family}_stage.c").read_text()
            for index in range(4):
                self.assertIn(f"out->tags[{index}]", client)
            # Six cells: id, four tags, the count. A count of five would mean
            # the array crossed as one value and the row would be misread.
            self.assertIn("caps, 6, NULL)", client)
            self.assertIn("row_slots[5] = row_text[0];", stage)
        finally:
            tmp.cleanup()

    def test_only_a_struct_member_may_repeat(self) -> None:
        # A repeating ARGUMENT is the repeated shape, which carries its own
        # count at the end of the frame. Letting a bare argument say `repeat`
        # would silently make its arity fixed.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            self.array_catalog(root)
            catalog = json.loads((root / contract.CATALOG).read_text(encoding="utf-8"))
            for operation in catalog["operations"]:
                if operation["name"] == "probe_tagged_get":
                    operation["request"]["fields"].append(
                        {"name": "terms", "type": "text", "required": True, "repeat": 3})
            self.write(root, catalog)
            with self.assertRaises(contract.ContractError):
                contract.run(root, write=False)
        finally:
            tmp.cleanup()

    def test_a_void_domain_gets_a_void_client(self) -> None:
        # A heartbeat answers nothing. Inventing a status for it would be a
        # value no caller checks and no domain produced -- and the stage cannot
        # assign one either, because there is nothing to assign from.
        client = (REPO_ROOT / contract.CLIENT_DIR / "delegation.c").read_text()
        stage = (REPO_ROOT / contract.SOURCE_DIR / "delegation_stage.c").read_text()
        body = client[client.index("void db1_agent_job_heartbeat(int job_id)"):]
        body = body[:body.index("\n}")]
        self.assertIn("(void)call_stage(", body)
        self.assertNotIn("return write_result", body)
        case = stage[stage.index("AGENT_JOB_HEARTBEAT:"):]
        case = case[:case.index("break;")]
        self.assertIn("      db1_agent_job_heartbeat(parsed0);", case)
        self.assertNotIn("rc = db1_agent_job_heartbeat", case)

    def test_an_allocated_member_is_allocated_and_released_on_both_sides(self) -> None:
        # The caller frees the row with the call it always used, so the client
        # has to be the one that allocated it. A failure part-way through must
        # not leave the caller holding memory it was never told about.
        client = (REPO_ROOT / contract.CLIENT_DIR / "delegation.c").read_text()
        stage = (REPO_ROOT / contract.SOURCE_DIR / "delegation_stage.c").read_text()
        body = client[client.index("int db1_agent_job_get(int job_id,"):]
        body = body[:body.index("\n}")]
        self.assertIn("out->prompt = malloc(", body)
        self.assertIn("free(out->prompt);", body)
        self.assertIn("realloc(out->prompt", body)
        # The stage releases what the DOMAIN allocated, after write_reply has
        # read it -- which is why the release is in the tail, not the case.
        self.assertIn("free(member_owned[slot]);", stage)
        self.assertIn("? row_db1_agent_job_t.prompt : \"\";", stage)

    def test_a_read_carries_its_own_return_when_it_says_so(self) -> None:
        # classify_stale always fills the buffer and separately answers whether
        # that state counts as stale. Deriving the answer from "did any text
        # arrive" says yes every time.
        client = (REPO_ROOT / contract.CLIENT_DIR / "delegation.c").read_text()
        body = client[client.index("int db1_agent_job_classify_stale("):]
        body = body[:body.index("\n}")]
        self.assertIn("caps, 2, NULL)", body)
        self.assertIn("strtol(slot_rc, NULL, 10)", body)
        self.assertNotIn("read_result(", body)

    def test_a_row_crosses_whole_or_not_at_all(self) -> None:
        # A member the catalog leaves out is not refused, it is never written:
        # the client leaves it as memset left it, and the caller cannot tell an
        # omitted field from one that really is empty. Regeneration makes that
        # permanent without a word, which is what this refuses.
        tmp = sandbox()
        try:
            root = Path(tmp.name)
            catalog = self.catalog(root)
            dropped = None
            for operation in catalog["operations"]:
                reply = operation["reply"]
                if "struct" in reply and len(reply["fields"]) > 2:
                    dropped = reply["fields"].pop(1)
                    break
            self.assertIsNotNone(dropped, "no struct-shaped reply to test with")
            self.write(root, catalog)
            with self.assertRaises(contract.ContractError) as caught:
                contract.run(root, write=False)
            self.assertIn("struct-members", str(caught.exception))
            self.assertIn(str(dropped["name"]), str(caught.exception))
        finally:
            tmp.cleanup()

    def test_a_struct_is_read_by_its_braces_not_by_a_regex(self) -> None:
        # Two structs in one header: ".*?" from the first "typedef struct {" to
        # the named closing brace swallows the first one whole, and its members
        # come back as members of the second.
        text = """
        typedef struct { char first[8]; int second; } earlier_t;
        typedef struct { char only[8]; } later_t;
        """
        self.assertEqual(contract.struct_body(text, "later_t").strip(), "char only[8];")
        self.assertIn("first", contract.struct_body(text, "earlier_t"))
        self.assertIsNone(contract.struct_body(text, "absent_t"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
