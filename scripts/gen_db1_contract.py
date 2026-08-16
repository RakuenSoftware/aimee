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
CLIENT_DIR = Path("src/db1_client")
STAGES_HEADER = Path("src/modules/db1/db1_stages.h")
# Served by the module process alone; the daemon never links these, and their
# absence from DB1_SRCS is the design rather than evidence of a migration.
# db1_time.c supplies now_utc, which every process defines for itself -- the
# daemon from util.c. In DB1_SRCS it would be a duplicate symbol there; out of
# the module it is an undefined one here.
MODULE_ONLY_SOURCES = frozenset({"module_adapter.c", "db1_time.c"})

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
WIRE_FORMATS = ("db1-keyed-blob-v1", "db1-fields-v2")
PAYLOADS = ("none", "state", "text", "int", "int64")
FIELD_TYPES = ("text", "int", "int64")


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
        family = keys(entry, {"id", "name", "event_kind", "active", "doc",
                              "client_doc", "covers", "sources", "retired_sources"},
                      f"families[{index-1}]")
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
        text(family["doc"], f"{name}.doc", 512)
        if not isinstance(family["client_doc"], str) or len(family["client_doc"]) > 512:
            fail("client-doc", f"{name}.client_doc must be a string of at most 512 chars")
        text(family["covers"], f"{name}.covers", 512)
        sources = family["sources"]
        if not isinstance(sources, list) or not sources or sources != sorted(set(sources)):
            fail("family-sources", f"{name}.sources must be sorted, unique and nonempty")
        for source in sources:
            if not isinstance(source, str) or not NAME.fullmatch(source):
                fail("family-source-name", f"{name} names invalid source {source!r}")
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
    # Active families must be a DENSE PREFIX, because the process contract
    # assigns a stage its event kind from its POSITION in the stages array
    # (4096 + ref*256 + ordinal) and requires those ordinals dense from one.
    # Reserving a kind per family therefore only holds if families activate in
    # id order: activating family 7 while 3 is reserved would hand family 7 the
    # kind reserved for family 3, silently.
    active = [family["id"] for family in families.values() if family["active"]]
    if sorted(active) != list(range(1, len(active) + 1)):
        fail("family-order",
             f"active families must be 1..N with no gaps, got {sorted(active)}; "
             f"renumber the family being activated, which is free while reserved")
    return families


def validate_operations(raw: object, families: dict[str, dict[str, object]]) -> list[dict[str, object]]:
    if not isinstance(raw, list) or not raw:
        fail("operations", "operations must be a nonempty array")
    seen: set[tuple[str, int]] = set()
    names: set[str] = set()
    order: list[tuple[int, int]] = []
    operations: list[dict[str, object]] = []
    for index, entry in enumerate(raw):
        allowed = {"family", "id", "name", "wire_format", "scope", "transaction",
                   "idempotency", "results", "request", "reply"}
        entry.pop("_field_types", None)
        if not isinstance(entry, dict) or not set(entry) <= allowed | {"c_name", "c_params"} or \
                not allowed <= set(entry):
            fail("keys", f"operations[{index}] keys differ from version 1")
        operation = entry
        if "c_name" in operation:
            symbol = text(operation["c_name"], f"operations[{index}].c_name", 96)
            if not NAME.fullmatch(symbol) or not symbol.startswith("db1_"):
                fail("c-name", f"operations[{index}] c_name {symbol!r} must be a db1_ symbol")
        if ("c_params" in operation) != ("c_name" in operation):
            fail("c-params", f"operations[{index}] must name c_params exactly with c_name")
        if "c_params" in operation:
            # The wire field names stay as they are -- the first is the scope key
            # by rule -- so the C signature carries its own names rather than
            # exporting "key" into a header that has always said repo_path.
            parameters = operation["c_params"]
            reply_shape = operation["reply"]
            request_shape = operation["request"]
            inbound = 1 if "struct" in request_shape else len(request_shape["fields"])
            if "list" in reply_shape:
                outbound = 1                      # T *out, however wide the rows
            elif "struct" in reply_shape:
                outbound = 1                      # T *out
            elif reply_shape["fields"]:
                outbound = 2                      # char *buf, size_t cap
            else:
                outbound = 0
            expected = inbound + outbound
            if not isinstance(parameters, list) or len(parameters) != expected or \
                    len(set(parameters)) != expected:
                fail("c-params",
                     f"operations[{index}] c_params must name {expected} distinct parameters")
            for parameter in parameters:
                if not isinstance(parameter, str) or not NAME.fullmatch(parameter):
                    fail("c-params", f"operations[{index}] invalid parameter {parameter!r}")
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

        request_keys = {"fields", "struct"} if "struct" in operation["request"] else {"fields"}
        request = keys(operation["request"], request_keys, f"{name}.request")
        if "struct" in request and not re.fullmatch(r"[a-z][a-z0-9_]*_t", str(request["struct"])):
            fail("request-struct", f"{name} request struct must be a _t type name")
        raw_fields = request["fields"]
        if not isinstance(raw_fields, list) or not raw_fields:
            fail("request-fields", f"{name} request must declare at least one field")
        fields = []
        for position, entry_field in enumerate(raw_fields):
            declared = keys(entry_field, {"name", "type", "required"},
                            f"{name}.request.fields[{position}]")
            if type(declared["required"]) is not bool:
                fail("field-required",
                     f"{name} field {declared['name']!r} required must be boolean")
            # A scope key that may be absent is not a scope key.
            if position == 0 and not declared["required"] and operation["scope"] != "global":
                fail("field-required",
                     f"{name} is scoped, so its first field cannot be optional")
            if declared["type"] not in FIELD_TYPES:
                fail("field-type",
                     f"{name} field {declared['name']!r} type must be one of {list(FIELD_TYPES)}")
            fields.append(str(declared["name"]))
        operation["_field_types"] = [str(f["type"]) for f in raw_fields]
        # A scoped operation must take its scoping key FIRST, because that key is
        # the boundary: DB1 rows belong to a conversation, session or repository,
        # and reading without one crosses it.
        #
        # A genuinely global lookup has no such key -- searching for a session by
        # prefix spans repositories by definition -- so rather than dress one up
        # as scoped, the catalog makes it say "global" out loud. The declaration
        # is the audit trail: unscoped access is visible in review instead of
        # hidden behind a field that is only conventionally a key.
        # A struct request takes its member names from the C type, so the scope
        # key is identified by position rather than by being spelled "key".
        # The rule is the same either way: a scoped operation carries its key
        # first, and reviewing that is reading which member comes first.
        if "struct" in request:
            pass
        elif operation["scope"] == "global":
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

        reply_keys = {"fields", "max_bytes"}
        if "struct" in operation["reply"]:
            reply_keys = reply_keys | {"struct"}
        if "list" in operation["reply"]:
            reply_keys = reply_keys | {"list"}
        reply = keys(operation["reply"], reply_keys, f"{name}.reply")
        if "struct" in reply and not re.fullmatch(r"[a-z][a-z0-9_]*_t", str(reply["struct"])):
            fail("reply-struct", f"{name} reply struct must be a _t type name")
        if "list" in reply:
            # A list is a struct repeated, so it says which C parameter receives
            # the rows, which one bounds them, and how many the stage will build.
            # The bound is the caller's, and it is also the allocation: a stage
            # that trusted it would let a caller ask for an arbitrary array.
            listed = keys(reply["list"], {"out", "bound", "max_rows"}, f"{name}.reply.list")
            if "struct" not in reply:
                fail("reply-list", f"{name} declares a list but no row struct")
            if "c_params" not in operation:
                fail("reply-list", f"{name} declares a list but names no C parameters")
            params = list(operation["c_params"])
            if str(listed["out"]) not in params:
                fail("reply-list", f"{name} list out {listed['out']!r} is not a C parameter")
            if str(listed["bound"]) not in params:
                fail("reply-list", f"{name} list bound {listed['bound']!r} is not a C parameter")
            if listed["out"] == listed["bound"]:
                fail("reply-list", f"{name} list out and bound must differ")
            # The remaining parameters map onto the request fields in order, so
            # the bound's position tells us which field must be the integer.
            inputs = [p for p in params if p != listed["out"]]
            if len(inputs) != len(raw_fields):
                fail("reply-list",
                     f"{name} has {len(inputs)} input parameters but {len(raw_fields)} "
                     f"request fields")
            at = inputs.index(str(listed["bound"]))
            if str(raw_fields[at]["type"]) != "int":
                fail("reply-list",
                     f"{name} list bound {listed['bound']!r} maps to request field "
                     f"{raw_fields[at]['name']!r}, which must be an int")
            integer(listed["max_rows"], f"{name}.reply.list.max_rows", 1, 4096)
        reply_fields = reply["fields"]
        if not isinstance(reply_fields, list):
            fail("reply-fields", f"{name} reply fields must be an array")
        for position, declared in enumerate(reply_fields):
            shape = keys(declared, {"name", "type"}, f"{name}.reply.fields[{position}]")
            if shape["type"] not in PAYLOADS or shape["type"] == "none":
                fail("reply-payload",
                     f"{name} reply field type must be one of {[p for p in PAYLOADS if p != 'none']}")
            if not NAME.fullmatch(str(shape["name"])):
                fail("reply-field-name", f"{name} invalid reply field {shape['name']!r}")
        max_bytes = integer(reply["max_bytes"], f"{name}.reply.max_bytes", 0, 1 << 20)
        if (not reply_fields) != (max_bytes == 0):
            fail("reply-bytes", f"{name} reply max_bytes must be zero exactly when it carries nothing")
        operations.append(operation)
    return operations


def validate_catalog(value: object) -> dict[str, object]:
    catalog = keys(value, {
        "schema_version", "module", "wire_version", "catalog_complete",
        "infrastructure_sources", "coupled_sources", "families", "result_codes",
        "operations",
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


def validate_source_map(root: Path, catalog: dict[str, object]) -> None:
    """Every DB1 source belongs to exactly one family, or to infrastructure.

    This is what makes the catalog a map rather than a wish list: a domain
    nobody claimed is a domain nobody is planning to move, and a domain claimed
    twice is two families expecting to own the same rows.

    Infrastructure is named separately because it has no callers to migrate --
    the connection, the schema, the write path and the module's own handler.
    """
    try:
        on_disk = {path.stem for path in (root / SOURCE_DIR).glob("*.c")}
    except OSError as exc:
        fail("unreadable", f"cannot list {SOURCE_DIR}: {exc}")

    # Generated wire is not a domain, so no family claims it -- but only the
    # files this generator actually emits are exempt, so a stray *_stage.c is
    # still an unclaimed source rather than a name that happens to look derived.
    on_disk -= {f"{family['name']}_stage" for family, _ in client_families(catalog)}

    infrastructure = catalog["infrastructure_sources"]
    if not isinstance(infrastructure, list) or infrastructure != sorted(set(infrastructure)):
        fail("infrastructure", "infrastructure_sources must be sorted and unique")

    families = catalog["families"]
    assert isinstance(families, dict)
    owner: dict[str, str] = {}
    for name, family in families.items():
        for source in family["sources"]:
            if source in owner:
                fail("source-duplicate",
                     f"{source!r} is claimed by both {owner[source]!r} and {name!r}")
            owner[source] = name
    for source in infrastructure:
        if source in owner:
            fail("source-duplicate",
                 f"{source!r} is both infrastructure and claimed by {owner[source]!r}")
        owner[source] = "(infrastructure)"

    for source in sorted(set(owner) - on_disk):
        fail("source-absent", f"{source!r} is claimed but is not in {SOURCE_DIR}")
    for source in sorted(on_disk - set(owner)):
        fail("source-unclaimed",
             f"{source!r} is in {SOURCE_DIR} but no family or infrastructure claims it")


def validate_coupled_sources(catalog: dict[str, object]) -> None:
    """Sources that must migrate together must sit in one family.

    A family is the unit that activates, so two halves of one ledger in two
    families is a plan to split them -- and the coupling here exists because
    splitting this particular ledger already cost paid-for work that could not
    be replayed.
    """
    groups = catalog["coupled_sources"]
    if not isinstance(groups, list):
        fail("coupled", "coupled_sources must be an array")
    families = catalog["families"]
    assert isinstance(families, dict)
    owner = {source: name for name, family in families.items() for source in family["sources"]}
    for index, group in enumerate(groups):
        entry = keys(group, {"sources", "reason"}, f"coupled_sources[{index}]")
        sources = entry["sources"]
        if not isinstance(sources, list) or len(sources) < 2 or sources != sorted(set(sources)):
            fail("coupled-sources",
                 f"coupled_sources[{index}].sources must be sorted, unique and hold at least two")
        text(entry["reason"], f"coupled_sources[{index}].reason", 512)
        holders = {owner.get(source) for source in sources}
        if None in holders:
            missing = sorted(s for s in sources if s not in owner)
            fail("coupled-unclaimed",
                 f"coupled_sources[{index}] names unclaimed source(s) {missing}")
        if len(holders) != 1:
            fail("coupled-split",
                 f"coupled_sources[{index}] is split across families {sorted(holders)}: "
                 f"{entry['reason']}")


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

    # Generated stage handlers serve the module and were never in the daemon, so
    # their absence from the link is not evidence of a migration.
    generated = {f"{family['name']}_stage.c" for family, _ in client_families(catalog)}
    unlinked = on_disk - linked - set(MODULE_ONLY_SOURCES) - generated
    for source in sorted(unlinked - set(claimed)):
        fail("retired-unclaimed",
             f"{source!r} is no longer linked into the daemon but no family retires it")


PREAMBLE = """/* Wire contract for the DB1 process's bounded stages.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit: add a family or an operation to the
 * catalog and regenerate, so the numbering and the wire cannot drift apart.
 *
 * DB1 is the server's SQLite store. It is becoming a module so that callers
 * reach it over the event bus instead of linking it, which is what the module
 * doctrine requires of state. The C implementation stays for now; only the
 * boundary is new. See docs/proposals/pending/db1-as-a-go-module.md.
 *
 * Event kinds are fixed by the process contract at 4096 + ref*256 + stage. DB1
 * declares principal ref {ref}, so these are not a free choice. */
#ifndef AIMEE_DB1_MODULE_API_H
#define AIMEE_DB1_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
"""

# One paragraph per wire format, emitted the first time a family uses it.
WIRE_FORMAT_DOC = {
    "db1-keyed-blob-v1": """   Request:  op(u32) | key_len(u32) | key | json_len(u32) | json
   Response: status(u32) | json_len(u32) | json
   Lengths are little-endian, matching the rest of the bus surface.""",
    "db1-fields-v2": """   Request:  op(u32) | field_count(u32) | (len(u32) | bytes) * field_count
   Response: status(u32) | field_count(u32) | (len(u32) | bytes) * field_count

   Counted in both directions. The first family fixed its request at exactly two
   fields, which suits a keyed blob and suits nothing with three, so the count is
   explicit here rather than implied by the op.

   The reply counts for the same reason the request does: an operation that
   answers with a row, or with a list of them, has somewhere to put the values.
   A reply carrying nothing sends a count of zero, and one carrying a single
   value sends a count of one -- the shape does not change with the arity.""",
}

HELPERS = """static inline void aimee_db1_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_db1_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

#endif /* AIMEE_DB1_MODULE_API_H */
"""


def wrap(prose: str, first: str, rest: str, width: int = 79) -> list[str]:
    """Reflow a catalog sentence into a C comment, deterministically."""
    words, lines, current = prose.split(), [], first
    for word in words:
        # The prefix already carries its own trailing space, so the first word on
        # a line is appended bare -- otherwise every comment opens "/*  Family".
        candidate = f"{current}{word}" if current in (first, rest) else f"{current} {word}"
        if len(candidate) > width and current not in (first, rest):
            lines.append(current.rstrip())
            current = f"{rest}{word}"
        else:
            current = candidate
    if current.strip():
        lines.append(current.rstrip())
    return lines


def define_block(pairs: list[tuple[str, str]]) -> list[str]:
    """#defines with their values aligned, the way the tree writes them."""
    if not pairs:
        return []
    width = max(len(name) for name, _ in pairs)
    return [f"#define {name.ljust(width)} {value}" for name, value in pairs]


def header_bytes(catalog: dict[str, object]) -> str:
    families = catalog["families"]
    operations = catalog["operations"]
    assert isinstance(families, dict) and isinstance(operations, list)
    by_family: dict[str, list[dict[str, object]]] = {}
    for operation in operations:
        by_family.setdefault(str(operation["family"]), []).append(operation)

    out = [PREAMBLE.replace("{ref}", str(PRINCIPAL_REF))]
    seen_formats: set[str] = set()
    field_max = 0
    fields_max = 0
    state_max = 0

    for family in sorted(families.values(), key=lambda f: int(f["id"])):
        if not family["active"]:
            # A constant for a family nothing serves would invite a caller to
            # speak an event with no listener.
            continue
        name = str(family["name"])
        upper = name.upper()
        own = by_family.get(name, [])
        block = [""]
        block.extend(wrap(f"Family {family['id']}: {family['doc']}", "/* ", " * "))
        wire = str(own[0]["wire_format"]) if own else ""
        if wire and wire not in seen_formats:
            # Described once, beside the first family that speaks it.
            seen_formats.add(wire)
            block.append(" *")
            for line in WIRE_FORMAT_DOC[wire].rstrip().split("\n"):
                block.append((" * " + line.strip()).rstrip() if line.strip() else " *")
        block[-1] += " */"
        block.append("")
        block.extend(define_block([
            (f"AIMEE_DB1_EVENT_{upper}", f"{family['event_kind']}u"),
            (f"AIMEE_DB1_STAGE_{upper}", f"{family['id']}u"),
        ]))
        if own:
            block.append("")
            block.extend(define_block([
                (f"AIMEE_DB1_OP_{str(o['name']).upper()}", f"{o['id']}u") for o in own
            ]))
        out.append("\n".join(block) + "\n")

        for operation in own:
            reply = operation["reply"]
            request = operation["request"]
            assert isinstance(reply, dict) and isinstance(request, dict)
            kinds = {str(f["type"]) for f in reply["fields"]}
            if "state" in kinds:
                state_max = max(state_max, int(reply["max_bytes"]))
            if "text" in kinds:
                field_max = max(field_max, int(reply["max_bytes"]))
            if operation["wire_format"] == "db1-fields-v2":
                fields_max = max(fields_max, len(request["fields"]))

    limits: list[tuple[str, str]] = []
    if state_max:
        limits.append(("AIMEE_DB1_STATE_MAX", f"{state_max}u"))
    if field_max:
        limits.append(("AIMEE_DB1_VALUE_MAX", f"{field_max}u"))
    if fields_max:
        limits.append(("AIMEE_DB1_FIELDS_MAX", f"{fields_max}u"))
    if limits:
        out.append("\n/* Wire bounds, carried from the catalog. VALUE_MAX is the widest\n"
                   "   reply a stage may build; FIELDS_MAX is the widest request arity, and\n"
                   "   sizes the decoder's pointer array. Requests are NOT capped: they carry\n"
                   "   prompts and documents, an in-process caller passes those whole, and the\n"
                   "   frame already bounds what arrived. */\n"
                   + "\n".join(define_block(limits)) + "\n")

    out.append("\n" + "\n".join(define_block(
        [(f"AIMEE_DB1_STATUS_{code.upper()}", f"{index}u")
         for index, code in enumerate(RESULT_CODES)])) + "\n")
    out.append("\n" + HELPERS)
    return "".join(out)


CLIENT_SCAFFOLD = """/* db1_client/{stem}.c: the {stem} family, reached over the bus.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
 *
 * Same functions, same contract, different side of the boundary: the daemon
 * links this instead of the DB1 domain, so nothing that calls these had to
 * change.
 *
 * It lives OUTSIDE modules/db1 deliberately. The module's descriptor owns every
 * .c beside it and compiles them into the DB1 process, so a client with these
 * names in that directory would be linked twice into the one binary that must
 * not have it -- once as the caller and once as the implementation.
{client_doc} *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits, and reflowing generated output would put the file and the
 * catalog permanently one reformat apart. */
/* clang-format off */
{header}

#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB1_{upper}_CALL_TIMEOUT_MS 2000

static void warn_unreachable(int reason)
{{
   static int warned;
   if (warned)
      return;
   warned = 1;
   /* Said once per process: enough to tell a store that is down from one that
      is quiet, without one line per call. The numeric
      aimee_module_call_result_t, not its name, so this does not pull the whole
      event-bus library in behind the client for one string. */
   LOG_WARN("db1.{stem}", "DB1 %s is unreachable (module call result %d)", "{family}",
            reason);
}}

/* Size the frame from the arguments themselves.

   These carry prompts, results and JSON documents, not just identifiers, and
   in-process callers have always passed them whole. A fixed cap here would
   refuse exactly those calls and return the same -1 as a broken store -- fine
   in a test with short strings, wrong the first time a real prompt arrives. The
   bus bounds the message instead. */
static int frame_size(const char *const *fields, uint32_t count, size_t *need_out)
{{
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {{
      /* Empty is legal on the wire: an optional field the caller left out
         travels as zero length. Which fields may be empty is the operation's
         business, checked before the frame is built. */
      if (!fields[i])
         return -1;
      size_t n = strlen(fields[i]);
      if (n > AIMEE_MODULE_MESSAGE_MAX_BODY - need - 4u)
         return -1;
      need += 4u + n;
   }}
   *need_out = need;
   return 0;
}}

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static void encode(uint8_t *out, uint32_t op, const char *const *fields, uint32_t count)
{{
   uint32_t at = 0;
   aimee_db1_put_u32(out + at, op);
   at += 4u;
   aimee_db1_put_u32(out + at, count);
   at += 4u;
   for (uint32_t i = 0; i < count; ++i)
   {{
      uint32_t n = (uint32_t)strlen(fields[i]);
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, fields[i], n);
      at += n;
   }}
}}

/* Returns the module's status, or -1 when the call never produced one. */
/* Fills up to `slots` reply values, each into the buffer and capacity the
   caller supplied. A write passes none; a read passes one; a row passes one per
   member; a list passes one per member per row it is willing to accept.

   `filled_out` reports how many values the reply actually carried, which is how
   a list learns its length: the rows are not counted separately on the wire
   because an operation already knows how wide its rows are. Callers that expect
   a fixed shape pass NULL. */
static int call_stage(uint32_t op, const char *const *fields, uint32_t count, char *const *values,
                      const size_t *caps, uint32_t slots, uint32_t *filled_out)
{{
   if (filled_out)
      *filled_out = 0u;
   for (uint32_t i = 0; i < slots; ++i)
      if (values[i] && caps[i])
         values[i][0] = '\\0';
   /* A local check, not a probe: with nothing serving the stage there is no
      call to make, and saying so beats waiting out a deadline. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_{upper}))
   {{
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return -1;
   }}

   size_t request_len = 0;
   if (frame_size(fields, count, &request_len) != 0)
      return -1;
   /* The reply is bounded by the caller's own buffer: it asked for at most
      value_len bytes, so there is no reason to hold more than that. */
   size_t response_cap = 8u;
   for (uint32_t i = 0; i < slots; ++i)
      response_cap += 4u + caps[i];
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   if (!request || !response)
   {{
      free(request);
      free(response);
      return -1;
   }}
   encode(request, op, fields, count);

   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_{upper}_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_{upper}, AIMEE_DB1_STAGE_{upper}, 0, deadline,
                           request, (uint32_t)request_len, response, (uint32_t)response_cap,
                           &response_len, NULL, NULL);
   free(request);

   int result = -1;
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
      warn_unreachable((int)rc);
   else
   {{
      uint32_t status = aimee_db1_get_u32(response);
      uint32_t fields_in = aimee_db1_get_u32(response + 4u);
      /* Read the reply's own count rather than assuming an arity: a status with
         no values is how a write answers, one value is a read, and a member
         apiece is a row. */
      result = (int)status;
      /* More values than the caller has room for is a contract mismatch, not
         something to read the first few of: the caller asked for at most this
         many rows, and a stage answering with more is not answering this call. */
      if (fields_in > slots)
         result = -1;
      else if (filled_out)
         *filled_out = fields_in;
      uint32_t at = 8u;
      for (uint32_t i = 0; i < fields_in && result != -1; ++i)
      {{
         if (at + 4u > response_len)
         {{
            result = -1;
            break;
         }}
         uint32_t n = aimee_db1_get_u32(response + at);
         at += 4u;
         /* A reply whose declared length runs past what arrived is not a reply
            to read part of. */
         if (at + n > response_len)
         {{
            result = -1;
            break;
         }}
         if (i < slots && values[i] && caps[i])
         {{
            /* No room for the terminator is no room: writing it would land one
               byte past the buffer the caller owns. */
            if (n >= caps[i])
               result = -1;
            else
            {{
               memcpy(values[i], response + at, n);
               values[i][n] = '\\0';
            }}
         }}
         at += n;
      }}
   }}
   free(response);
   return result;
}}

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}}

{read_result}"""


def domain_headers(root: Path, operations: list[dict[str, object]]) -> list[str]:
    """The DB1 headers that declare these operations.

    Derived rather than declared: the family name and the source name coincided
    for the first family and do not in general, so a header named after the
    family is a header that does not exist.
    """
    found: set[str] = set()
    for header in sorted((root / SOURCE_DIR).glob("*.h")):
        text = header.read_text(errors="ignore")
        for operation in operations:
            symbol = str(operation.get("c_name", ""))
            if symbol and re.search(r"\b" + re.escape(symbol) + r"\s*\(", text):
                found.add(header.name)
    return sorted(found)


def client_bytes(catalog: dict[str, object], family: dict[str, object],
                 operations: list[dict[str, object]], headers: list[str]) -> str:
    """Render one family's C client.

    Every body is the same three steps -- reject unusable arguments, name the
    fields, map the status -- which is exactly why writing 347 of them by hand
    was never the plan.
    """
    name = str(family["name"])
    upper = name.upper()
    doc = str(family["client_doc"]).strip()
    prose = ""
    if doc:
        prose = "\n *\n" + "\n".join(wrap(doc, " * ", " * "))
    includes = "\n".join(f'#include "{h}"' for h in headers)
    # Emitted only where something reads a single value: a family of writes and
    # rows has no use for it, and an unused static is a -Werror failure.
    plain_read = any("struct" not in o["reply"] and o["reply"]["fields"] for o in operations)
    reader = ("""/* A read answers found(1) / not-found(0) / error(-1), which is what the direct
   implementation returns and what its callers already branch on. */
static int read_result(int status, const char *value_out)
{
   if (status == (int)AIMEE_DB1_STATUS_OK)
      return (value_out && value_out[0]) ? 1 : 0;
   if (status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   return -1;
}
""" if plain_read else "")
    out = [CLIENT_SCAFFOLD.format(
        stem=name, family=name.replace("_", " "), upper=upper,
        header=includes, client_doc=prose, read_result=reader)]

    for operation in operations:
        request = operation["request"]
        reply = operation["reply"]
        assert isinstance(request, dict) and isinstance(reply, dict)
        fields = [str(f["name"]) for f in request["fields"]]
        types = [str(f["type"]) for f in request["fields"]]
        reads = bool(reply["fields"])
        required = [bool(f.get("required", True)) for f in request["fields"]]
        names = [str(n) for n in operation["c_params"]]
        in_struct = str(request["struct"]) if "struct" in request else ""
        out_struct = str(reply["struct"]) if "struct" in reply else ""
        listed = reply.get("list")
        if listed:
            # The rows parameter can sit anywhere in the signature -- some
            # domains put it first -- so the C order is read from c_params and
            # only the remaining parameters map onto the fields, in order.
            row_out = str(listed["out"])
            bound = str(listed["bound"])
            inputs = [p for p in names if p != row_out]
            outputs = [row_out]
        else:
            split = 1 if in_struct else len(fields)
            inputs, outputs = names[:split], names[split:]

        if in_struct:
            # The struct is the argument; its members are the frame.
            params = [f"const {in_struct} *{inputs[0]}"]
            guards = [f"!{inputs[0]}"]
        else:
            params = [(f"int {p}" if t in ("int", "int64") else f"const char *{p}")
                      for p, t in zip(inputs, types)]
            # An int cannot be null and has no empty case, so only text is
            # guarded -- and only where the operation says it must be there.
            guards = [f"!{p} || !{p}[0]" for p, t, need in zip(inputs, types, required)
                      if t == "text" and need]
        if listed:
            # Re-order to the C signature: the declarations above are in field
            # order, which is the same order minus the rows parameter.
            declared = dict(zip(inputs, params))
            declared[row_out] = f"{out_struct} *{row_out}"
            params = [declared[p] for p in names]
            guards += [f"!{row_out}", f"{bound} <= 0"]
        elif out_struct:
            params += [f"{out_struct} *{outputs[0]}"]
            guards += [f"!{outputs[0]}"]
        elif reads:
            params += [f"char *{outputs[0]}", f"size_t {outputs[1]}"]
            guards += [f"!{outputs[0]}", f"{outputs[1]} == 0"]
        signature = f"int {operation['c_name']}({', '.join(params)})"
        body = [signature, "{"]
        if guards:
            body += [f"   if ({' || '.join(guards)})", "      return -1;"]
        if listed:
            # Clamped rather than refused, because the domain clamps too: this
            # ceiling is the one the implementation already enforces, so a
            # caller asking for more has always been given fewer. Refusing here
            # would break a caller whose array is simply larger than the query
            # can fill. The clamp precedes the frame so the stage is told the
            # bound it will actually honour.
            body += [f"   if ({bound} > {listed['max_rows']})",
                     f"      {bound} = {listed['max_rows']};"]
        # Integers travel as decimal text: the frame carries counted bytes, and a
        # separate numeric type on the wire would buy nothing a printf does not.
        carried = []
        sources = ([f"{inputs[0]}->{f}" for f in fields] if in_struct else list(inputs))
        for position, (source, kind, need) in enumerate(zip(sources, types, required)):
            local = f"arg{position}"
            if kind == "text" and not need and not in_struct:
                # The domains already read NULL as empty; the wire says so too
                # rather than refusing a caller that leaves a value out.
                carried.append(f"{source} ? {source} : \"\"")
                continue
            if kind in ("int", "int64"):
                spec = "%lld" if kind == "int64" else "%d"
                cast = "(long long)" if kind == "int64" else ""
                body.append(f"   char {local}[24];")
                body.append(f'   snprintf({local}, sizeof {local}, "{spec}", {cast}{source});')
                carried.append(local)
            else:
                carried.append(source)
        body.append(f"   const char *fields[] = {{{', '.join(carried)}}};")
        op_symbol = f"AIMEE_DB1_OP_{str(operation['name']).upper()}"
        arity = len(fields)
        if listed:
            members = [(str(f["name"]), str(f["type"])) for f in reply["fields"]]
            width = len(members)
            numeric = [i for i, (_, k) in enumerate(members) if k in ("int", "int64")]
            body.append(f"   char **values = malloc((size_t){bound} * {width}u * sizeof *values);")
            body.append(f"   size_t *caps = malloc((size_t){bound} * {width}u * sizeof *caps);")
            if numeric:
                body.append(f"   char (*scratch)[24] = malloc((size_t){bound} * {len(numeric)}u * "
                            "sizeof *scratch);")
            owned = "values, caps" + (", scratch" if numeric else "")
            checks = " || ".join(f"!{o}" for o in owned.split(", "))
            body.append(f"   if ({checks})")
            body.append("   {")
            for item in owned.split(", "):
                body.append(f"      free({item});")
            body.append("      return -1;")
            body.append("   }")
            body.append(f"   memset({row_out}, 0, (size_t){bound} * sizeof *{row_out});")
            body.append(f"   for (int row = 0; row < {bound}; ++row)")
            body.append("   {")
            slot = 0
            for index, (member, kind) in enumerate(members):
                at = f"row * {width}u + {index}u"
                if kind in ("int", "int64"):
                    cell = f"scratch[row * {len(numeric)}u + {slot}u]"
                    body.append(f"      values[{at}] = {cell};")
                    body.append(f"      caps[{at}] = sizeof {cell};")
                    slot += 1
                else:
                    body.append(f"      values[{at}] = {row_out}[row].{member};")
                    body.append(f"      caps[{at}] = sizeof {row_out}[row].{member};")
            body.append("   }")
            body.append("   uint32_t filled = 0;")
            body.append(f"   int status = call_stage({op_symbol}, fields, {arity}, values, caps,")
            body.append(f"                           (uint32_t)({bound} * {width}), &filled);")
            body.append("   free(values);")
            body.append("   free(caps);")
            fail_free = "".join(f"\n      free({o});" for o in (["scratch"] if numeric else []))
            # A reply that is not a whole number of rows is not this operation's
            # reply, whatever its status says.
            body.append(f"   if (status != (int)AIMEE_DB1_STATUS_OK || filled % {width}u != 0u)")
            body.append("   {" + fail_free)
            body.append("      return -1;")
            body.append("   }")
            body.append(f"   int rows = (int)(filled / {width}u);")
            if numeric:
                body.append("   for (int row = 0; row < rows; ++row)")
                body.append("   {")
                slot = 0
                for member, kind in members:
                    if kind in ("int", "int64"):
                        cell = f"scratch[row * {len(numeric)}u + {slot}u]"
                        conv = ("(int)strtol" if kind == "int" else "(int64_t)strtoll")
                        body.append(f"      {row_out}[row].{member} = {conv}({cell}, NULL, 10);")
                        slot += 1
                body.append("   }")
                body.append("   free(scratch);")
            body.append("   return rows;")
        elif out_struct:
            members = [(str(f["name"]), str(f["type"])) for f in reply["fields"]]
            target = outputs[0]
            # A numeric member is read as text and converted, the same way it was
            # sent: the frame carries bytes, and a row is only its members.
            for index, (member, kind) in enumerate(members):
                if kind in ("int", "int64"):
                    body.append(f"   char slot{index}[24];")
            slots = ", ".join(
                f"slot{index}" if kind in ("int", "int64") else f"{target}->{member}"
                for index, (member, kind) in enumerate(members))
            caps = ", ".join(
                f"sizeof slot{index}" if kind in ("int", "int64")
                else f"sizeof {target}->{member}"
                for index, (member, kind) in enumerate(members))
            body.append(f"   char *const values[] = {{{slots}}};")
            body.append(f"   const size_t caps[] = {{{caps}}};")
            body.append(f"   memset({target}, 0, sizeof *{target});")
            body.append(f"   int status = call_stage({op_symbol}, fields, {arity}, values, caps, "
                        f"{len(members)}, NULL);")
            # The domain answers 0 or -1 here: a miss and a failure are the
            # same answer to its callers, and the wire does not invent a
            # distinction the contract never had.
            body.append("   if (status != (int)AIMEE_DB1_STATUS_OK)")
            body.append("      return -1;")
            for index, (member, kind) in enumerate(members):
                if kind == "int":
                    body.append(f"   {target}->{member} = (int)strtol(slot{index}, NULL, 10);")
                elif kind == "int64":
                    body.append(f"   {target}->{member} = (int64_t)strtoll(slot{index}, NULL, 10);")
            body.append("   return 0;")
        elif reads:
            body.append(f"   char *const values[] = {{{outputs[0]}}};")
            body.append(f"   const size_t caps[] = {{{outputs[1]}}};")
            body.append(f"   int status = call_stage({op_symbol}, fields, {arity}, values, caps, 1, NULL);")
            body.append(f"   return read_result(status, {outputs[0]});")
        else:
            body.append(f"   return write_result(call_stage({op_symbol}, fields, {arity}, "
                        f"NULL, NULL, 0, NULL));")
        body.append("}")
        out.append("\n" + "\n".join(body) + "\n")
    out.append("\n/* clang-format on */\n")
    return "".join(out)


def client_families(catalog: dict[str, object]) -> list[tuple[dict, list]]:
    """Families whose whole operation set names a C symbol, in id order."""
    families = catalog["families"]
    operations = catalog["operations"]
    assert isinstance(families, dict) and isinstance(operations, list)
    result = []
    for family in sorted(families.values(), key=lambda f: int(f["id"])):
        own = [o for o in operations if o["family"] == family["name"]]
        if own and all("c_name" in o for o in own):
            result.append((family, own))
        elif any("c_name" in o for o in own):
            fail("client-partial",
                 f"{family['name']} names a C symbol for some operations but not all")
    return result


def validate_clients(root: Path, catalog: dict[str, object], write: bool) -> None:
    for family, operations in client_families(catalog):
        path = root / CLIENT_DIR / f"{family['name']}.c"
        expected = client_bytes(catalog, family, operations,
                                domain_headers(root, operations))
        if write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(expected, encoding="utf-8")
        try:
            actual = path.read_text(encoding="utf-8")
        except OSError as exc:
            fail("client-missing", f"cannot read {path}: {exc}")
        if actual != expected:
            fail("client-stale",
                 f"{path} is not what the catalog generates; run "
                 f"scripts/gen_db1_contract.py --write")


STAGE_SCAFFOLD = """/* modules/db1/{stem}_stage.c: the {family} stage handler.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
 *
 * The serving half of the boundary: decode the frame the client encoded, call
 * the domain, and answer. The domain itself is hand-written and untouched --
 * only the wire around it is generated.
 *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits. */
/* clang-format off */
#include "db1_stages.h"

#include "db1_module_api.h"
{headers}

{int_includes}#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy one counted field out of the frame, NUL-terminated.

   The frame bounds the field, not a fixed cap: these carry prompts, results and
   JSON documents, and an in-process caller has always passed them whole. An
   embedded NUL is still refused -- every field is spliced into a query
   parameter, and a NUL would silently shorten it into a different row. */
static int read_counted(const uint8_t *body, uint32_t len, uint32_t *offset, char **cursor,
                        const char **out)
{{
   if (*offset + 4u > len)
      return 1;
   uint32_t n = aimee_db1_get_u32(body + *offset);
   *offset += 4u;
   if (n > len || *offset + n > len)
      return 1;
   if (memchr(body + *offset, 0, n) != NULL)
      return 1;
   memcpy(*cursor, body + *offset, n);
   (*cursor)[n] = '\\0';
   *out = *cursor;
   *cursor += n + 1u;
   *offset += n;
   return 0;
}}

{parse_int}{parse_int64}/* status(u32) | field_count(u32) | (len(u32) | bytes) * count. A write answers
   with no values, a read with one, a row with a value per member. */
static uint32_t write_reply(uint8_t *out, uint32_t cap, uint32_t *out_len, uint32_t status,
                            const char *const *values, uint32_t count)
{{
   uint32_t at = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {{
      uint32_t n = (uint32_t)strlen(values[i]);
      if (cap < at + 4u + n)
         return AIMEE_DB1_STATUS_FAILED;
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      if (n)
         memcpy(out + at, values[i], n);
      at += n;
   }}
   if (cap < 8u)
      return AIMEE_DB1_STATUS_FAILED;
   aimee_db1_put_u32(out, status);
   aimee_db1_put_u32(out + 4u, count);
   *out_len = at;
   return status;
}}

aimee_module_status_t aimee_db1_stage_{stem}(const uint8_t *request_body, uint32_t request_len,
                                             uint8_t *response_body, uint32_t response_capacity,
                                             uint32_t *response_len)
{{
   if (request_len < 8u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   uint32_t op = aimee_db1_get_u32(request_body);
   uint32_t count = aimee_db1_get_u32(request_body + 4u);
   /* Bounds the fixed array below. Without it a well-formed frame declaring
      more fields than any operation takes writes past it. */
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   /* One allocation for every field, sized by the frame that carried them: the
      fields plus a NUL each cannot exceed this. */
   const char *field[AIMEE_DB1_FIELDS_MAX];
   char *scratch = malloc((size_t)request_len + AIMEE_DB1_FIELDS_MAX);
   if (!scratch)
      return AIMEE_MODULE_STATUS_INTERNAL;
   char *cursor = scratch;
   aimee_module_status_t decoded = AIMEE_MODULE_STATUS_OK;

   uint32_t offset = 8u;
   for (uint32_t i = 0; i < count; ++i)
      if (read_counted(request_body, request_len, &offset, &cursor, &field[i]) != 0)
         decoded = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   /* Trailing bytes mean the caller and the module disagree about the op's
      arity, which is a contract mismatch rather than something to tolerate. */
   if (offset != request_len)
      decoded = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (decoded != AIMEE_MODULE_STATUS_OK)
   {{
      free(scratch);
      return decoded;
   }}

   char value[AIMEE_DB1_VALUE_MAX];
   value[0] = '\\0';
   int rc = -1;
   int reads = 0;
   /* A row answers with a value per member; a plain read answers with one; a
      list answers with a value per member per row. */
   const char *const *rows = NULL;
   uint32_t row_count = 0u;
   /* A list returns its length in rc, where a read returns found/not-found, so
      the two cannot share a status mapping. The three owned blocks below hold
      the domain's rows, the cell pointers into them and the text for numeric
      members: all three must outlive write_reply, because that is what reads
      them. Declared unconditionally so this stays one readable flow -- unlike
      the static helpers above, an unused local costs nothing. */
   int listed = 0;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;

   switch (op)
   {{
{cases}   default:
      free(scratch);
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }}
   free(scratch);

   /* The two conventions must not be flattened. A read returns FOUND(1),
      not-found(0) or error(-1); a write returns 0 or -1. Mapping a read's -1
      onto MISSING would report a broken store as "nothing recorded", and the
      caller would act on an absence that was never established. */
   uint32_t status;
   if (listed)
      /* A list answers with how many rows it found, so any count is success and
         only a negative return is a failure. Zero rows is an empty list, not a
         miss: the caller asked what was there and the answer was nothing. */
      status = (rc >= 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (rows)
      /* A row-returning domain answers 0 or -1: there is no found/not-found
         distinction to preserve, so neither is invented. */
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (reads)
   {{
      if (rc < 0)
         status = AIMEE_DB1_STATUS_FAILED;
      else if (rc == 0 || !value[0])
         status = AIMEE_DB1_STATUS_MISSING;
      else
         status = AIMEE_DB1_STATUS_OK;
   }}
   else
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;

   {{
      const char *one = (status == AIMEE_DB1_STATUS_OK) ? value : "";
      const char *const single[] = {{one}};
      const char *const *out_values = rows ? rows : (reads ? single : NULL);
      uint32_t out_count = rows ? row_count : (reads ? 1u : 0u);
      if (status != AIMEE_DB1_STATUS_OK && rows)
         out_count = 0u; /* nothing to report but the status */
      write_reply(response_body, response_capacity, response_len, status, out_values, out_count);
   }}
   free(cells_owned);
   free(numeric_owned);
   free(domain_rows);
   return AIMEE_MODULE_STATUS_OK;
}}
/* clang-format on */
"""


PARSE_INT = """/* Parse a field the catalog declared as an integer. Refuses anything that is
   not exactly a number: a partial parse would turn "12abc" into 12 and act on a
   value the caller never sent. */
static int parse_int(const char *text, int *out)
{{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   long value = strtol(text, &end, 10);
   if (errno != 0 || !end || *end != '\\0' || value < INT_MIN || value > INT_MAX)
      return 1;
   *out = (int)value;
   return 0;
}}
"""

PARSE_INT64 = """/* The same, for a member the catalog declared as a 64-bit integer. */
static int parse_int64(const char *text, int64_t *out)
{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   long long value = strtoll(text, &end, 10);
   if (errno != 0 || !end || *end != '\\0')
      return 1;
   *out = (int64_t)value;
   return 0;
}

"""

INT_INCLUDES = """#include <errno.h>
#include <limits.h>
#include <stdint.h>
"""


def stage_bytes(family: dict[str, object], operations: list[dict[str, object]],
                headers: list[str]) -> str:
    name = str(family["name"])
    cases = []
    for operation in operations:
        request = operation["request"]
        reply = operation["reply"]
        assert isinstance(request, dict) and isinstance(reply, dict)
        arity = len(request["fields"])
        types = [str(f["type"]) for f in request["fields"]]
        names = [str(f["name"]) for f in request["fields"]]
        reads = bool(reply["fields"])
        in_struct = str(request["struct"]) if "struct" in request else ""
        out_struct = str(reply["struct"]) if "struct" in reply else ""
        parse, args, tail = [], [], []

        if in_struct:
            # Rebuild the row the caller flattened, then hand the domain the
            # struct it has always taken.
            parse.append(f"      {in_struct} row;\n      memset(&row, 0, sizeof row);\n")
            for position, (member, kind) in enumerate(zip(names, types)):
                if kind in ("int", "int64"):
                    conv = "parse_int" if kind == "int" else "parse_int64"
                    parse.append(f"      if ({conv}(field[{position}], &row.{member}) != 0)\n"
                                 f"      {{\n         free(scratch);\n"
                                 f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                                 f"      }}\n")
                else:
                    parse.append(f"      snprintf(row.{member}, sizeof row.{member}, \"%s\", "
                                 f"field[{position}]);\n")
            args.append("&row")
        else:
            for position, kind in enumerate(types):
                if kind in ("int", "int64"):
                    conv = "parse_int" if kind == "int" else "parse_int64"
                    ctype = "int" if kind == "int" else "int64_t"
                    args.append(f"parsed{position}")
                    parse.append(f"      {ctype} parsed{position};\n"
                                 f"      if ({conv}(field[{position}], &parsed{position}) != 0)\n"
                                 f"      {{\n"
                                 f"         free(scratch);\n"
                                 f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                                 f"      }}\n")
                else:
                    args.append(f"field[{position}]")

        listed = reply.get("list")
        if listed:
            members = [(str(f["name"]), str(f["type"])) for f in reply["fields"]]
            width = len(members)
            numeric = [i for i, (_, k) in enumerate(members) if k in ("int", "int64")]
            row_out = str(listed["out"])
            bound = str(listed["bound"])
            inputs = [p for p in operation["c_params"] if p != row_out]
            at = inputs.index(bound)
            held = args[at]
            # The bound is the allocation, so it is checked against the ceiling
            # the catalog declares before anything is allocated from it. A stage
            # that took the caller's word would size an array from the wire.
            parse.append(f"      if ({held} <= 0 || {held} > {listed['max_rows']})\n"
                         f"      {{\n         free(scratch);\n"
                         f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n      }}\n")
            parse.append(f"      {out_struct} *found = calloc((size_t){held}, sizeof *found);\n"
                         f"      if (!found)\n"
                         f"      {{\n         free(scratch);\n"
                         f"         return AIMEE_MODULE_STATUS_INTERNAL;\n      }}\n"
                         f"      domain_rows = found;\n")
            ordered = dict(zip(inputs, args))
            ordered[row_out] = "found"
            args = [ordered[p] for p in operation["c_params"]]
            assigns = "".join(
                f"            cells[row * {width}u + {i}u] = "
                + (f"numbers[row * {len(numeric)}u + {numeric.index(i)}u];\n"
                   if kind in ("int", "int64") else f"found[row].{member};\n")
                for i, (member, kind) in enumerate(members))
            converts = "".join(
                f"            snprintf(numbers[row * {len(numeric)}u + {numeric.index(i)}u], 24,\n"
                f"                     \"{'%lld' if kind == 'int64' else '%d'}\", "
                f"{'(long long)' if kind == 'int64' else ''}found[row].{member});\n"
                for i, (member, kind) in enumerate(members) if kind in ("int", "int64"))
            numbers = (f"         char (*numbers)[24] = malloc((size_t)produced * "
                       f"{len(numeric)}u * sizeof *numbers);\n" if numeric else "")
            guard = "!cells" + (" || !numbers" if numeric else "")
            release = "            free(cells);\n" + ("            free(numbers);\n" if numeric else "")
            tail.append(
                "      if (rc > 0)\n"
                "      {\n"
                # A domain that answered with more rows than it was given would
                # otherwise be read past the end of its own array. No test kills
                # a mutant here and none can from outside: it guards against the
                # domain breaking its own contract, which the real domain does
                # not do. Kept because the failure it prevents is a heap
                # over-read, and the cost is one comparison.
                f"         uint32_t produced = ((uint32_t)rc < (uint32_t){held})\n"
                f"                                 ? (uint32_t)rc : (uint32_t){held};\n"
                f"         const char **cells = malloc((size_t)produced * {width}u * sizeof *cells);\n"
                + numbers
                + f"         if ({guard})\n"
                "         {\n"
                + release
                + "            free(scratch);\n"
                "            free(domain_rows);\n"
                "            return AIMEE_MODULE_STATUS_INTERNAL;\n"
                "         }\n"
                "         cells_owned = cells;\n"
                + ("         numeric_owned = numbers;\n" if numeric else "")
                + "         for (uint32_t row = 0; row < produced; ++row)\n"
                "         {\n"
                + converts
                + assigns
                + "         }\n"
                "         rows = cells;\n"
                f"         row_count = produced * {width}u;\n"
                "      }\n"
                "      listed = 1;\n")
        elif out_struct:
            members = [(str(f["name"]), str(f["type"])) for f in reply["fields"]]
            parse.append(f"      {out_struct} out;\n      memset(&out, 0, sizeof out);\n")
            args.append("&out")
            for index, (member, kind) in enumerate(members):
                if kind in ("int", "int64"):
                    spec = "%lld" if kind == "int64" else "%d"
                    cast = "(long long)" if kind == "int64" else ""
                    tail.append(f"      char text{index}[24];\n"
                                f"      snprintf(text{index}, sizeof text{index}, \"{spec}\", "
                                f"{cast}out.{member});\n")
            emitted = ", ".join(f"text{index}" if kind in ("int", "int64") else f"out.{member}"
                                for index, (member, kind) in enumerate(members))
            tail.append(f"      const char *const row_values[] = {{{emitted}}};\n")
            tail.append(f"      rows = row_values;\n      row_count = {len(members)}u;\n")
        elif reads:
            args += ["value", "sizeof value"]
        # Braced only when a parsed integer needs scoping: an empty block around
        # every other case would be noise, and would move files that have not
        # changed.
        body = (f"      if (count != {arity}u)\n"
                f"      {{\n"
                f"         free(scratch);\n"
                f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                f"      }}\n"
                + "".join(parse)
                + f"      rc = {operation['c_name']}({', '.join(args)});\n"
                + "".join(tail)
                # Not for a list: it declares member fields like a row does, but
                # an empty list must answer with no values, where a read answers
                # with one. Setting this would turn "nothing found" into a
                # single empty string.
                + ("      reads = 1;\n" if reads and not listed else "")
                + "      break;\n")
        head = f"   case AIMEE_DB1_OP_{str(operation['name']).upper()}:\n"
        needs = "".join(
            f"      if (!field[{i}][0])\n      {{\n         free(scratch);\n"
            f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n      }}\n"
            for i, f in enumerate(request["fields"]) if f["required"])
        marker = "         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n      }\n"
        cut = body.index(marker) + len(marker)
        body = body[:cut] + needs + body[cut:]
        cases.append(head + (f"   {{\n{body}   }}\n" if parse else body))
    used = {str(f["type"]) for o in operations for f in o["request"]["fields"]}
    typed = bool(used & {"int", "int64"})
    return STAGE_SCAFFOLD.format(stem=name, family=name.replace("_", " "),
                                 headers="\n".join(f'#include "{h}"' for h in headers),
                                 cases="".join(cases),
                                 parse_int=PARSE_INT if "int" in used else "",
                                 parse_int64=PARSE_INT64 if "int64" in used else "",
                                 int_includes=INT_INCLUDES if typed else "")


def stages_header_bytes(catalog: dict[str, object]) -> str:
    """Declarations for every generated stage handler, for the adapter to call."""
    lines = ["""/* Entry points for the generated DB1 stage handlers.
 *
 * GENERATED from src/modules/db1/eventcontract/operations.json by
 * scripts/gen_db1_contract.py. Do not edit.
 *
 * module_adapter.c dispatches by stage and calls these; each is emitted beside
 * the domain it serves.
 *
 * clang-format is off below: the canonical form is whatever this emits. */
/* clang-format off */
#ifndef AIMEE_DB1_STAGES_H
#define AIMEE_DB1_STAGES_H 1

#include <aimee/core/event_bus/module_runtime.h>

#include <stdint.h>
"""]
    for family, _ in client_families(catalog):
        name = str(family["name"])
        lines.append(
            f"\naimee_module_status_t aimee_db1_stage_{name}("
            "const uint8_t *request_body, uint32_t request_len,\n"
            f"{' ' * (len(name) + 30)}uint8_t *response_body,\n"
            f"{' ' * (len(name) + 30)}uint32_t response_capacity,\n"
            f"{' ' * (len(name) + 30)}uint32_t *response_len);\n")
    lines.append("\n#endif /* AIMEE_DB1_STAGES_H */\n/* clang-format on */\n")
    return "".join(lines)


def validate_stages(root: Path, catalog: dict[str, object], write: bool) -> None:
    wanted = {(root / SOURCE_DIR / f"{family['name']}_stage.c"):
              stage_bytes(family, operations, domain_headers(root, operations))
              for family, operations in client_families(catalog)}
    wanted[root / STAGES_HEADER] = stages_header_bytes(catalog)
    for path, expected in wanted.items():
        if write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(expected, encoding="utf-8")
        try:
            actual = path.read_text(encoding="utf-8")
        except OSError as exc:
            fail("stage-missing", f"cannot read {path}: {exc}")
        if actual != expected:
            fail("stage-stale",
                 f"{path} is not what the catalog generates; run "
                 f"scripts/gen_db1_contract.py --write")


def validate_header(root: Path, catalog: dict[str, object]) -> None:
    """The header must be exactly what the catalog generates.

    A whole-file comparison rather than a constant-by-constant one: it also
    catches a constant for a family nothing serves, a stale limit, and hand
    edits that the per-symbol checks would have walked straight past.
    """
    expected = header_bytes(catalog)
    try:
        actual = (root / HEADER).read_text(encoding="utf-8")
    except OSError as exc:
        fail("unreadable", f"cannot read {HEADER}: {exc}")
    if actual != expected:
        fail("header-stale",
             f"{HEADER} is not what the catalog generates; run "
             f"scripts/gen_db1_contract.py --write")


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


def validate_dispatch(root: Path, catalog: dict[str, object]) -> None:
    """Every active family's stage must be reachable from the adapter.

    The generator writes the stage; the adapter's switch is hand-written,
    because the first family answers a different wire format and is served by a
    hand-written handler. Nothing connected the two, so activating a family
    produced a stage that compiled, linked, passed its own tests and could not
    be called: the runtime invoked the stage id and the switch fell through to
    its default. That is what happened to conversation, and it went unnoticed
    because a family with no dispatched stage looks exactly like a family whose
    callers have not cut over yet.
    """
    adapter = (root / "src/modules/db1/module_adapter.c").read_text(encoding="utf-8")
    families = catalog["families"]
    assert isinstance(families, dict)
    for family in families.values():
        if not family["active"]:
            continue
        label = f"case AIMEE_DB1_STAGE_{str(family['name']).upper()}:"
        if label not in adapter:
            fail("stage-undispatched",
                 f"module_adapter.c has no {label} -- an active family whose stage "
                 f"nothing routes to is a stage no caller can reach")


def run(root: Path, write: bool = False) -> None:
    catalog = validate_catalog(load_json(root / CATALOG))
    if write:
        (root / HEADER).write_text(header_bytes(catalog), encoding="utf-8")
    validate_header(root, catalog)
    validate_clients(root, catalog, write)
    validate_stages(root, catalog, write)
    validate_dispatch(root, catalog)
    validate_process_contract(root, catalog)
    validate_source_map(root, catalog)
    validate_coupled_sources(catalog)
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
    parser.add_argument("--write", action="store_true",
                        help="regenerate the wire header from the catalog")
    args = parser.parse_args(argv)
    try:
        run(args.root.resolve(), args.write)
    except ContractError as exc:
        print(f"gen_db1_contract: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
