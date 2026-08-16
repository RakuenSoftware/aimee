#!/usr/bin/env python3
"""Validate the DB1 operation catalog against the wire header and process contract.

DB1 is becoming a module one domain at a time. Each domain is a FAMILY that owns
one event kind, and the operations inside it dispatch on an op id in the payload
-- the same shape DB2 uses, so the two stores stay legible side by side.

Families are declared UP FRONT and activated as their callers move onto the bus.
An inactive family is a reservation, not a commitment: it pins an event kind so
the numbering cannot shift under a migration that has already shipped, and it
says which DB1 sources it will cover so the remaining work is countable rather
than discovered. Renaming or regrouping an inactive family is therefore fine.
Changing an ACTIVE one is not: callers are already speaking it.

This validates rather than generates. The wire header is still hand-written, and
the catalog's job today is to be the source of truth that header is checked
against -- drift between the two is what this exists to catch. Generation can
follow once more than one family is active and the duplication is real.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import NoReturn

ROOT = Path(__file__).resolve().parent.parent
CATALOG = Path("src/modules/db1/eventcontract/operations.json")
HEADER = Path("src/modules/db1/db1_module_api.h")
PROCESS_CONTRACTS = Path("src/modules/process-contracts.json")
MAKEFILE = Path("src/Makefile")
SOURCE_DIR = Path("src/modules/db1")
# Served by the module process alone; the daemon never links it.
MODULE_ONLY_SOURCES = frozenset({"module_adapter.c"})

# DB1's principal ref. Event kinds are carved 4096 + ref*256 + stage, and a
# family's id IS its future stage id, so the arithmetic is fixed here too.
PRINCIPAL_REF = 30
KIND_BASE = 4096 + PRINCIPAL_REF * 256

MAX_BYTES = 1_048_576
NAME = re.compile(r"^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$")
RESULT_CODES = ("ok", "missing", "invalid", "too_long", "failed")
SCOPES = ("none", "conversation", "session", "repository", "global")
TRANSACTIONS = ("none", "single")
IDEMPOTENCY = ("safe", "idempotent", "unsafe")
WIRE_FORMATS = ("db1-keyed-blob-v1", "db1-fields-v1")
PAYLOADS = ("none", "state", "text")


class ContractError(ValueError):
    """A fail-closed catalog or drift error."""


def fail(rule: str, message: str) -> NoReturn:
    raise ContractError(f"rule={rule}: {message}")


def load_json(path: Path) -> object:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("unreadable", f"cannot read {path}: {exc}")
    if len(raw) > MAX_BYTES:
        fail("oversize", f"{path} exceeds {MAX_BYTES} bytes")
    if raw.startswith(b"\xef\xbb\xbf"):
        fail("bom", f"{path} begins with a UTF-8 BOM")

    def no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        seen: dict[str, object] = {}
        for key, value in pairs:
            if key in seen:
                fail("duplicate-key", f"{path} repeats key {key!r}")
            seen[key] = value
        return seen

    try:
        return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=no_duplicates)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        fail("parse", f"cannot parse {path}: {exc}")


def keys(value: object, expected: set[str], label: str) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != expected:
        fail("keys", f"{label} keys differ from version 1: {sorted(expected)}")
    return value


def integer(value: object, label: str, low: int, high: int) -> int:
    if type(value) is not int or not low <= value <= high:
        fail("integer", f"{label} must be an integer in [{low}, {high}]")
    return value


def text(value: object, label: str, maximum: int = 256) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        fail("string", f"{label} must be a nonempty string of at most {maximum} chars")
    return value


def validate_families(raw: object) -> dict[str, dict[str, object]]:
    if not isinstance(raw, list) or not raw:
        fail("families", "families must be a nonempty array")
    families: dict[str, dict[str, object]] = {}
    for index, entry in enumerate(raw, start=1):
        family = keys(entry, {"id", "name", "event_kind", "active", "covers",
                              "retired_sources"}, f"families[{index-1}]")
        # Dense from one, because a family id is its future stage id and stage
        # IDs must be dense from one in the process contract.
        if integer(family["id"], f"families[{index-1}].id", 1, 255) != index:
            fail("family-id", f"family {index} must declare id {index}")
        name = text(family["name"], f"families[{index-1}].name", 64)
        if not NAME.fullmatch(name):
            fail("family-name", f"invalid family name {name!r}")
        if name in families:
            fail("family-duplicate", f"duplicate family {name!r}")
        expected_kind = KIND_BASE + index
        if integer(family["event_kind"], f"{name}.event_kind", 1, 65535) != expected_kind:
            fail("family-event-kind",
                 f"{name} event_kind must equal {expected_kind} (4096 + {PRINCIPAL_REF}*256 + {index})")
        if type(family["active"]) is not bool:
            fail("family-active-type", f"{name}.active must be boolean")
        # A reservation states what it will cover, so the remaining migration is
        # countable from the catalog instead of rediscovered each time. This is
        # a PLAN, in file names, and it is deliberately not machine-checked: a
        # source can hold more than one domain, so "covers" over-states what a
        # family has actually taken. retired_sources is the checked half.
        text(family["covers"], f"{name}.covers", 512)
        retired = family["retired_sources"]
        if not isinstance(retired, list) or retired != sorted(set(retired)):
            fail("retired-sources", f"{name}.retired_sources must be sorted and unique")
        for entry_name in retired:
            if not isinstance(entry_name, str) or not entry_name.endswith(".c") or \
                    "/" in entry_name or not NAME.fullmatch(entry_name[:-2]):
                fail("retired-source-name", f"{name} names invalid source {entry_name!r}")
        if retired and not family["active"]:
            fail("retired-reserved",
                 f"reserved family {name!r} claims retired sources, but nothing serves it")
        families[name] = family
    if not any(family["active"] for family in families.values()):
        fail("family-active", "at least one family must be active")
    return families


def validate_operations(raw: object, families: dict[str, dict[str, object]]) -> list[dict[str, object]]:
    if not isinstance(raw, list) or not raw:
        fail("operations", "operations must be a nonempty array")
    seen: set[tuple[str, int]] = set()
    names: set[str] = set()
    order: list[tuple[int, int]] = []
    operations: list[dict[str, object]] = []
    for index, entry in enumerate(raw):
        operation = keys(entry, {
            "family", "id", "name", "wire_format", "scope", "transaction",
            "idempotency", "results", "request", "reply",
        }, f"operations[{index}]")
        family_name = operation["family"]
        if not isinstance(family_name, str) or family_name not in families:
            fail("operation-family", f"operations[{index}] names unknown family {family_name!r}")
        # An operation on a reserved family would be a contract nothing serves.
        if not families[family_name]["active"]:
            fail("operation-inactive",
                 f"operations[{index}] declares an operation on inactive family {family_name!r}")
        identifier = integer(operation["id"], f"operations[{index}].id", 1, 0xffffffff)
        name = text(operation["name"], f"operations[{index}].name", 64)
        if not NAME.fullmatch(name):
            fail("operation-name", f"invalid operation name {name!r}")
        key = (family_name, identifier)
        if key in seen or name in names:
            fail("operation-duplicate", f"duplicate operation {key!r}/{name!r}")
        seen.add(key)
        names.add(name)
        position = (int(families[family_name]["id"]), identifier)
        if order and position <= order[-1]:
            fail("operation-order", "operations must be sorted by family id then operation id")
        order.append(position)

        if operation["wire_format"] not in WIRE_FORMATS:
            fail("wire-format", f"{name} wire_format must be one of {list(WIRE_FORMATS)}")
        if operation["scope"] not in SCOPES:
            fail("scope", f"{name} scope must be one of {list(SCOPES)}")
        if operation["transaction"] not in TRANSACTIONS:
            fail("transaction", f"{name} transaction must be one of {list(TRANSACTIONS)}")
        if operation["idempotency"] not in IDEMPOTENCY:
            fail("idempotency", f"{name} idempotency must be one of {list(IDEMPOTENCY)}")

        results = operation["results"]
        if not isinstance(results, list) or not results:
            fail("results", f"{name} results must be a nonempty array")
        if "ok" not in results:
            fail("results-ok", f"{name} must be able to succeed")
        for result in results:
            if result not in RESULT_CODES:
                fail("result-code", f"{name} declares unknown result {result!r}")
        if results != sorted(results, key=RESULT_CODES.index):
            fail("results-order", f"{name} results must follow the declared result order")

        request = keys(operation["request"], {"fields"}, f"{name}.request")
        fields = request["fields"]
        if not isinstance(fields, list) or not fields:
            fail("request-fields", f"{name} request must declare at least one field")
        # A scoped operation must take its scoping key FIRST, because that key is
        # the boundary: DB1 rows belong to a conversation, session or repository,
        # and reading without one crosses it.
        #
        # A genuinely global lookup has no such key -- searching for a session by
        # prefix spans repositories by definition -- so rather than dress one up
        # as scoped, the catalog makes it say "global" out loud. The declaration
        # is the audit trail: unscoped access is visible in review instead of
        # hidden behind a field that is only conventionally a key.
        if operation["scope"] == "global":
            if fields[0] == "key":
                fail("request-global",
                     f"{name} is declared global but takes a key; scope it instead")
        elif operation["scope"] != "none" and fields[0] != "key":
            fail("request-key", f"{name} is scoped, so it must take its key first")
        for field in fields:
            if not isinstance(field, str) or not NAME.fullmatch(field):
                fail("request-field-name", f"{name} declares invalid request field {field!r}")
        if len(set(fields)) != len(fields):
            fail("request-field-duplicate", f"{name} repeats a request field")

        reply = keys(operation["reply"], {"payload", "max_bytes"}, f"{name}.reply")
        if reply["payload"] not in PAYLOADS:
            fail("reply-payload", f"{name} reply payload must be one of {list(PAYLOADS)}")
        max_bytes = integer(reply["max_bytes"], f"{name}.reply.max_bytes", 0, 1 << 20)
        if (reply["payload"] == "none") != (max_bytes == 0):
            fail("reply-bytes", f"{name} reply max_bytes must be zero exactly when it carries nothing")
        operations.append(operation)
    return operations


def validate_catalog(value: object) -> dict[str, object]:
    catalog = keys(value, {
        "schema_version", "module", "wire_version", "catalog_complete", "families",
        "result_codes", "operations",
    }, "catalog")
    if catalog["schema_version"] != 1:
        fail("schema-version", "schema_version must equal 1")
    if catalog["module"] != "db1":
        fail("module", "module must equal 'db1'")
    if catalog["wire_version"] != 1:
        fail("wire-version", "wire_version must equal 1")
    if type(catalog["catalog_complete"]) is not bool:
        fail("catalog-complete-type", "catalog_complete must be boolean")
    if catalog["result_codes"] != list(RESULT_CODES):
        fail("result-codes", "result_codes must equal the closed version-1 result set")
    families = validate_families(catalog["families"])
    operations = validate_operations(catalog["operations"], families)
    # Completeness is a claim about DB1's whole surface, so it cannot be true
    # while families are still reserved for callers that have not moved.
    if catalog["catalog_complete"] and not all(f["active"] for f in families.values()):
        fail("catalog-complete",
             "catalog_complete cannot be true while a family is still reserved")
    catalog["families"] = families
    catalog["operations"] = operations
    return catalog


def validate_retired_sources(root: Path, catalog: dict[str, object]) -> None:
    """A source the daemon stopped linking must be claimed by an active family.

    "covers" is a plan and can over-state: a DB1 source often holds more than one
    domain, and family 1 took the economizer's reducer state out of checkpoints.c
    while the rest of that file still serves callers in-process. So the claim
    that is actually enforced is the narrow one -- this source is no longer
    linked into the daemon, because its callers reach it over the bus.

    Checked in both directions on purpose. A family cannot claim a source the
    daemon still links, and a source cannot quietly leave the daemon's link
    without a family owning it. Without the second half, a migration could drop
    a domain out of the binary and leave nothing saying where it went.
    """
    try:
        makefile = (root / MAKEFILE).read_text(encoding="utf-8")
    except OSError as exc:
        fail("unreadable", f"cannot read {MAKEFILE}: {exc}")
    try:
        on_disk = {path.name for path in (root / SOURCE_DIR).glob("*.c")}
    except OSError as exc:
        fail("unreadable", f"cannot list {SOURCE_DIR}: {exc}")

    linked = {name for name in on_disk if f"modules/db1/{name}" in makefile}
    families = catalog["families"]
    assert isinstance(families, dict)

    claimed: dict[str, str] = {}
    for name, family in families.items():
        for source in family["retired_sources"]:
            if source not in on_disk:
                fail("retired-missing", f"{name} retires {source!r}, which is not in {SOURCE_DIR}")
            if source in linked:
                fail("retired-still-linked",
                     f"{name} retires {source!r}, but {MAKEFILE} still links it")
            if source in claimed:
                fail("retired-duplicate",
                     f"{source!r} is retired by both {claimed[source]!r} and {name!r}")
            claimed[source] = name

    unlinked = on_disk - linked - set(MODULE_ONLY_SOURCES)
    for source in sorted(unlinked - set(claimed)):
        fail("retired-unclaimed",
             f"{source!r} is no longer linked into the daemon but no family retires it")


def header_constants(root: Path) -> dict[str, int]:
    try:
        text_value = (root / HEADER).read_text(encoding="utf-8")
    except OSError as exc:
        fail("unreadable", f"cannot read {HEADER}: {exc}")
    found: dict[str, int] = {}
    for match in re.finditer(r"^#define\s+(AIMEE_DB1_[A-Z0-9_]+)\s+(\d+)u?\s*$",
                             text_value, re.M):
        found[match.group(1)] = int(match.group(2))
    return found


def validate_header(root: Path, catalog: dict[str, object]) -> None:
    """The hand-written wire header must agree with the catalog, in both directions."""
    constants = header_constants(root)
    families = catalog["families"]
    assert isinstance(families, dict)
    for name, family in families.items():
        if not family["active"]:
            # A reserved family must NOT have wire constants yet: a constant is
            # a promise to callers, and nothing serves this one.
            leaked = [key for key in constants if name.upper() in key]
            if leaked:
                fail("header-reserved",
                     f"reserved family {name!r} already has wire constants {leaked}")
            continue
        event = f"AIMEE_DB1_EVENT_{name.upper()}"
        stage = f"AIMEE_DB1_STAGE_{name.upper()}"
        if constants.get(event) != family["event_kind"]:
            fail("header-event",
                 f"{HEADER} must define {event} as {family['event_kind']}")
        if constants.get(stage) != family["id"]:
            fail("header-stage", f"{HEADER} must define {stage} as {family['id']}")

    operations = catalog["operations"]
    assert isinstance(operations, list)
    for operation in operations:
        symbol = f"AIMEE_DB1_OP_{str(operation['name']).upper()}"
        if constants.get(symbol) != operation["id"]:
            fail("header-op", f"{HEADER} must define {symbol} as {operation['id']}")

    for index, code in enumerate(RESULT_CODES):
        symbol = f"AIMEE_DB1_STATUS_{code.upper()}"
        if constants.get(symbol) != index:
            fail("header-status", f"{HEADER} must define {symbol} as {index}")


def validate_process_contract(root: Path, catalog: dict[str, object]) -> None:
    """Active families are exactly DB1's declared stages, with the same kinds."""
    contract = load_json(root / PROCESS_CONTRACTS)
    if not isinstance(contract, dict):
        fail("process-contract", f"{PROCESS_CONTRACTS} must be an object")
    component = next((c for c in contract.get("components", [])
                      if isinstance(c, dict) and c.get("id") == "db1"), None)
    if component is None:
        fail("process-contract", "db1 is absent from the process contract")
    if component.get("principal_ref") != PRINCIPAL_REF:
        fail("principal-ref", f"db1 principal_ref must equal {PRINCIPAL_REF}")

    families = catalog["families"]
    assert isinstance(families, dict)
    active = {name: family for name, family in families.items() if family["active"]}
    stages = component.get("stages")
    if not isinstance(stages, list):
        fail("process-contract", "db1 stages must be an array")
    if len(stages) != len(active):
        fail("stage-count",
             f"db1 declares {len(stages)} stage(s) for {len(active)} active famil(ies)")
    for stage in stages:
        if not isinstance(stage, dict):
            fail("process-contract", "db1 stage entries must be objects")
        name = str(stage.get("name", ""))
        # Stage names are hyphenated and module-prefixed; families are not.
        family_name = name.removeprefix("db1-").replace("-", "_")
        family = active.get(family_name)
        if family is None:
            fail("stage-family", f"stage {name!r} has no active family in the catalog")
        if stage.get("event_kind") != family["event_kind"] or stage.get("id") != family["id"]:
            fail("stage-binding",
                 f"stage {name!r} must carry id {family['id']} and kind {family['event_kind']}")


def run(root: Path) -> None:
    catalog = validate_catalog(load_json(root / CATALOG))
    validate_header(root, catalog)
    validate_process_contract(root, catalog)
    validate_retired_sources(root, catalog)
    families = catalog["families"]
    operations = catalog["operations"]
    assert isinstance(families, dict) and isinstance(operations, list)
    active = sum(1 for family in families.values() if family["active"])
    retired = sum(len(family["retired_sources"]) for family in families.values())
    print(f"gen_db1_contract: ok ({len(families)} famil(ies), {active} active, "
          f"{len(operations)} operation(s), {retired} source(s) off the daemon)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        run(args.root.resolve())
    except ContractError as exc:
        print(f"gen_db1_contract: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
