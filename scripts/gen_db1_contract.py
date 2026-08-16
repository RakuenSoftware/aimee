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
            reads = operation["reply"]["payload"] != "none"
            expected = len(operation["request"]["fields"]) + (2 if reads else 0)
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
    "db1-fields-v1": """   Request:  op(u32) | field_count(u32) | (len(u32) | bytes) * field_count
   Response: status(u32) | value_len(u32) | value

   The counted form is the one every family after the first uses. The first
   family fixed its request at exactly two fields, which suits a keyed blob and
   suits nothing with three, so the count is explicit here rather than implied
   by the op.""",
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
            if reply["payload"] == "state":
                state_max = max(state_max, int(reply["max_bytes"]))
            if reply["payload"] == "text":
                field_max = max(field_max, int(reply["max_bytes"]))
            if operation["wire_format"] == "db1-fields-v1":
                fields_max = max(fields_max, len(request["fields"]))

    limits: list[tuple[str, str]] = []
    if state_max:
        limits.append(("AIMEE_DB1_STATE_MAX", f"{state_max}u"))
    if field_max:
        limits.append(("AIMEE_DB1_FIELD_MAX", f"{field_max}u"))
    if fields_max:
        limits.append(("AIMEE_DB1_FIELDS_MAX", f"{fields_max}u"))
    if limits:
        out.append("\n/* Wire bounds, carried from the catalog's declared reply sizes and\n"
                   "   request arities. Stated so the module refuses an over-long value rather\n"
                   "   than truncating one into something that looks valid. */\n"
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
#include "{header}"

#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include "log.h"
#include "module_json_call.h"

#include <stdio.h>
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

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static int encode(uint8_t *out, size_t out_sz, uint32_t op, const char *const *fields,
                  uint32_t count, uint32_t *len_out)
{{
   if (count == 0u || count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {{
      if (!fields[i] || !fields[i][0])
         return -1;
      size_t n = strlen(fields[i]);
      /* Refuse here rather than let the module refuse: an over-long field is a
         caller bug, and the round trip would only rename it. */
      if (n >= AIMEE_DB1_FIELD_MAX)
         return -1;
      need += 4u + n;
   }}
   if (need > out_sz)
      return -1;

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
   *len_out = at;
   return 0;
}}

/* Returns the module's status, or -1 when the call never produced one. */
static int call_stage(uint32_t op, const char *const *fields, uint32_t count, char *value_out,
                      size_t value_len)
{{
   if (value_out && value_len)
      value_out[0] = '\\0';
   /* A local check, not a probe: with nothing serving the stage there is no
      call to make, and saying so beats waiting out a deadline. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_{upper}))
   {{
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return -1;
   }}

   uint8_t request[8u + AIMEE_DB1_FIELDS_MAX * (4u + AIMEE_DB1_FIELD_MAX)];
   uint32_t request_len = 0;
   if (encode(request, sizeof request, op, fields, count, &request_len) != 0)
      return -1;

   uint8_t response[8u + AIMEE_DB1_FIELD_MAX];
   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_{upper}_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_{upper}, AIMEE_DB1_STAGE_{upper}, 0, deadline,
                           request, request_len, response, (uint32_t)sizeof response,
                           &response_len, NULL, NULL);
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
   {{
      warn_unreachable((int)rc);
      return -1;
   }}

   uint32_t status = aimee_db1_get_u32(response);
   uint32_t payload_len = aimee_db1_get_u32(response + 4u);
   /* A reply whose declared length disagrees with what arrived is not a reply
      to read part of. */
   if (payload_len != response_len - 8u)
      return -1;
   if (value_out && value_len)
   {{
      if (payload_len >= value_len)
         return -1;
      memcpy(value_out, response + 8u, payload_len);
      value_out[payload_len] = '\\0';
   }}
   return (int)status;
}}

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}}

/* A read answers found(1) / not-found(0) / error(-1), which is what the direct
   implementation returns and what its callers already branch on. */
static int read_result(int status, char *value_out)
{{
   if (status == (int)AIMEE_DB1_STATUS_OK)
      return (value_out && value_out[0]) ? 1 : 0;
   if (status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   return -1;
}}
"""


def client_bytes(catalog: dict[str, object], family: dict[str, object],
                 operations: list[dict[str, object]]) -> str:
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
    out = [CLIENT_SCAFFOLD.format(
        stem=name, family=name.replace("_", " "), upper=upper,
        header=f"{name}.h", client_doc=prose)]

    for operation in operations:
        request = operation["request"]
        reply = operation["reply"]
        assert isinstance(request, dict) and isinstance(reply, dict)
        fields = [str(f) for f in request["fields"]]
        reads = reply["payload"] != "none"
        names = [str(n) for n in operation["c_params"]]
        inputs, outputs = names[:len(fields)], names[len(fields):]
        params = [f"const char *{parameter}" for parameter in inputs]
        if reads:
            params += [f"char *{outputs[0]}", f"size_t {outputs[1]}"]
        guards = [f"!{parameter}" for parameter in inputs]
        if reads:
            guards += [f"!{outputs[0]}", f"{outputs[1]} == 0"]

        signature = f"int {operation['c_name']}({', '.join(params)})"
        body = [signature, "{",
                f"   if ({' || '.join(guards)})", "      return -1;",
                f"   const char *fields[] = {{{', '.join(inputs)}}};"]
        op_symbol = f"AIMEE_DB1_OP_{str(operation['name']).upper()}"
        if reads:
            body.append(f"   int status = call_stage({op_symbol}, fields, "
                        f"{len(fields)}, {outputs[0]}, {outputs[1]});")
            body.append(f"   return read_result(status, {outputs[0]});")
        else:
            body.append(f"   return write_result(call_stage({op_symbol}, fields, "
                        f"{len(fields)}, NULL, 0));")
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
        expected = client_bytes(catalog, family, operations)
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
#include "{stem}.h"

#include <string.h>

/* Read one counted field, refusing anything that would run past the end or
   carry an embedded NUL: every field here is spliced into a query parameter,
   and a NUL would silently shorten it into a different row. */
static int read_counted(const uint8_t *body, uint32_t len, uint32_t *offset, char *out,
                        size_t out_sz)
{{
   if (*offset + 4u > len)
      return 1;
   uint32_t n = aimee_db1_get_u32(body + *offset);
   *offset += 4u;
   if (n > len || *offset + n > len || n == 0u || n >= out_sz)
      return 1;
   if (memchr(body + *offset, 0, n) != NULL)
      return 1;
   memcpy(out, body + *offset, n);
   out[n] = '\\0';
   *offset += n;
   return 0;
}}

static uint32_t write_reply(uint8_t *out, uint32_t cap, uint32_t *out_len, uint32_t status,
                            const char *value)
{{
   uint32_t value_len = (uint32_t)strlen(value);
   if (cap < 8u + value_len)
      return AIMEE_DB1_STATUS_FAILED;
   aimee_db1_put_u32(out, status);
   aimee_db1_put_u32(out + 4u, value_len);
   if (value_len)
      memcpy(out + 8u, value, value_len);
   *out_len = 8u + value_len;
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

   char field[AIMEE_DB1_FIELDS_MAX][AIMEE_DB1_FIELD_MAX];
   uint32_t offset = 8u;
   for (uint32_t i = 0; i < count; ++i)
      if (read_counted(request_body, request_len, &offset, field[i], sizeof field[i]) != 0)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   /* Trailing bytes mean the caller and the module disagree about the op's
      arity, which is a contract mismatch rather than something to tolerate. */
   if (offset != request_len)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   char value[AIMEE_DB1_FIELD_MAX];
   value[0] = '\\0';
   int rc = -1;
   int reads = 0;

   switch (op)
   {{
{cases}   default:
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   }}

   /* The two conventions must not be flattened. A read returns FOUND(1),
      not-found(0) or error(-1); a write returns 0 or -1. Mapping a read's -1
      onto MISSING would report a broken store as "nothing recorded", and the
      caller would act on an absence that was never established. */
   uint32_t status;
   if (reads)
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

   write_reply(response_body, response_capacity, response_len,
               status, (status == AIMEE_DB1_STATUS_OK && reads) ? value : "");
   return AIMEE_MODULE_STATUS_OK;
}}
/* clang-format on */
"""


def stage_bytes(family: dict[str, object], operations: list[dict[str, object]]) -> str:
    name = str(family["name"])
    cases = []
    for operation in operations:
        request = operation["request"]
        reply = operation["reply"]
        assert isinstance(request, dict) and isinstance(reply, dict)
        arity = len(request["fields"])
        reads = reply["payload"] != "none"
        args = [f"field[{i}]" for i in range(arity)]
        if reads:
            args += ["value", "sizeof value"]
        cases.append(
            f"   case AIMEE_DB1_OP_{str(operation['name']).upper()}:\n"
            f"      if (count != {arity}u)\n"
            f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
            f"      rc = {operation['c_name']}({', '.join(args)});\n"
            + ("      reads = 1;\n" if reads else "")
            + "      break;\n")
    return STAGE_SCAFFOLD.format(stem=name, family=name.replace("_", " "),
                                 cases="".join(cases))


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
    wanted = {(root / SOURCE_DIR / f"{family['name']}_stage.c"): stage_bytes(family, operations)
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


def run(root: Path, write: bool = False) -> None:
    catalog = validate_catalog(load_json(root / CATALOG))
    if write:
        (root / HEADER).write_text(header_bytes(catalog), encoding="utf-8")
    validate_header(root, catalog)
    validate_clients(root, catalog, write)
    validate_stages(root, catalog, write)
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
