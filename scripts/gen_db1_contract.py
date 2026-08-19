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
# the module it is an undefined one here. db1_module_init.c opens the store the
# module serves from, which the daemon opens for itself.
MODULE_ONLY_SOURCES = frozenset({"module_adapter.c", "db1_time.c", "db1_module_init.c",
                                 "db1_module_support.c"})

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
PAYLOADS = ("none", "state", "text", "int", "int64", "uint64", "double", "float", "struct")
# A request carries a double for the same reason a reply does: a cost is a
# number, and rounding it to an integer at the boundary would bill differently
# on each side of it. The conversion is the one the reply already uses.
FIELD_TYPES = ("text", "int", "int64", "uint64", "double", "float")
# Members that travel as decimal text and convert back on arrival.
NUMERIC = ("int", "int64", "uint64", "double", "float")


# Widest decimal text a member can become, with room for the NUL. A %.17g
# double reaches 24 characters, which is exactly what a 24-byte slot cannot
# hold: the value would arrive truncated into a different number.
NUMERIC_TEXT = 32


def numeric_format(kind: str) -> tuple[str, str]:
    """printf spec and cast for a member that travels as decimal text.

    %.17g for a double because that is the shortest form guaranteed to read
    back as the same IEEE-754 value; %g alone rounds to six significant digits
    and would quietly change a cost or a rate on its way across.
    """
    if kind == "int64":
        return "%lld", "(long long)"
    if kind == "uint64":
        # A content hash uses the whole width. Rendered signed it comes back as
        # the same bits on a two's-complement machine and as a different number
        # to anyone reading the frame, so it is rendered as what it is.
        return "%llu", "(unsigned long long)"
    if kind == "double":
        return "%.17g", "(double)"
    if kind == "float":
        # %.9g is the shortest form that reads back as the same IEEE-754
        # single, the way %.17g is for a double.
        return "%.9g", "(double)"
    return "%d", ""


def numeric_parse(kind: str, cell: str) -> str:
    """The whole C call that converts one such member back.

    The call, not just the function: strtod takes two arguments where strtol
    and strtoll take three, and a helper that returned only the name invited
    pairing it with the wrong tail -- which compiles nowhere but was generated
    once anyway.
    """
    if kind == "int64":
        return f"(int64_t)strtoll({cell}, NULL, 10)"
    if kind == "uint64":
        return f"(uint64_t)strtoull({cell}, NULL, 10)"
    if kind == "float":
        return f"(float)strtod({cell}, NULL)"
    if kind == "double":
        return f"strtod({cell}, NULL)"
    return f"(int)strtol({cell}, NULL, 10)"


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


# A field name, or one element of an expanded array or nested member: "qa[3]",
# "hypothesis.content", "qa[3].answer" -- the way C spells each of them.
ELEMENT = re.compile(r"[a-z][a-z0-9_]*(\[[0-9]+\])?(\.[a-z][a-z0-9_]*(\[[0-9]+\])?)?")


def expand_repeats(fields: list[dict[str, object]]) -> list[dict[str, object]]:
    """Turn a `char member[N][W]` declaration into the N values it holds.

    The alternative is variable arity, and a struct member does not have any:
    the array is always N wide, and the slots the domain left empty are empty
    strings rather than absent. Naming the expansion `member[i]` means every
    emitter that writes `row.{name}` or `sizeof out->{name}` keeps working --
    C spells an element exactly that way -- so this is the whole capability.
    """
    grown = []
    for field in fields:
        # A member that is itself a struct expands to its own members, spelled
        # the way C spells them. Same trick as the array: every emitter that
        # writes row.{name} keeps working because "outer.inner" IS that.
        nested = field.get("fields")
        if nested is not None and "repeat" in field:
            for index in range(int(field["repeat"])):
                for member in nested:
                    # A member that is itself an array expands again, and
                    # "outer[i].inner[j]" is how C spells that too, so the
                    # emitters keep working for the same reason.
                    span = range(int(member["repeat"])) if "repeat" in member else (None,)
                    for at in span:
                        inner = {k: v for k, v in member.items() if k != "repeat"}
                        inner["name"] = (f"{field['name']}[{index}].{member['name']}"
                                         + ("" if at is None else f"[{at}]"))
                        if "required" in field:
                            inner.setdefault("required", field["required"])
                        grown.append(inner)
            continue
        if nested is not None and "repeat" not in field:
            for member in nested:
                inner = dict(member)
                inner["name"] = f"{field['name']}.{member['name']}"
                if "required" in field:
                    inner.setdefault("required", field["required"])
                grown.append(inner)
            continue
        repeat = field.get("repeat")
        if repeat is None:
            grown.append(field)
            continue
        for index in range(int(repeat)):
            element = {k: v for k, v in field.items() if k != "repeat"}
            element["name"] = f"{field['name']}[{index}]"
            grown.append(element)
    return grown


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


def validate_operations(raw: object, families: dict[str, dict[str, object]],
                        root: Path) -> list[dict[str, object]]:
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
        if not isinstance(entry, dict) or \
                not set(entry) <= allowed | {"c_name", "c_params", "c_returns",
                                            "c_member", "negatives"} or \
                not allowed <= set(entry):
            fail("keys", f"operations[{index}] keys differ from version 1")
        operation = entry
        if "c_name" in operation:
            symbol = text(operation["c_name"], f"operations[{index}].c_name", 96)
            if not NAME.fullmatch(symbol) or not symbol.startswith("db1_"):
                fail("c-name", f"operations[{index}] c_name {symbol!r} must be a db1_ symbol")
        if ("c_params" in operation) != ("c_name" in operation):
            fail("c-params", f"operations[{index}] must name c_params exactly with c_name")
        # "int"   0 succeeded, negative failed -- a write, or a read whose
        #          absence is indistinguishable from an error
        # "found"  1 found, 0 nothing there, negative failed -- the domain makes
        #          the distinction, so the wire has to carry it
        # "text"   a malloc'd string, NULL for nothing
        # "int64"  a new row id, negative on failure. The id IS the answer, so
        #          it has to cross rather than be flattened to success.
        # "member"  the row is filled AND one of its members is handed back --
        #          a claim, where the caller gets the task and its id at once.
        #          c_member says which member; nothing-there answers negative,
        #          because the domains that do this use the id as the flag.
        # "void"   the domain answers nothing at all -- a heartbeat, a status
        #          nudge. Inventing a return here would be a status its callers
        #          never had and cannot check.
        # "rc"     a read whose buffer and whose return are separate answers:
        #          classify_stale fills in "idle" and returns whether that
        #          counts as stale. Reconstructing one from the other -- as a
        #          plain read does, by asking whether any text arrived -- gives
        #          the wrong answer whenever the text is always there.
        if operation.get("c_returns", "int") not in ("int", "found", "text", "int64",
                                                     "member", "void", "rc"):
            fail("c-returns",
                 f"operations[{index}] c_returns must be \"int\", \"found\", \"int64\", "
                 f"\"member\", \"void\", \"rc\" or \"text\"")
        if ("c_member" in operation) != (operation.get("c_returns") == "member"):
            fail("c-returns",
                 f"operations[{index}] names c_member exactly when it returns a member")
        if "c_returns" in operation and "c_name" not in operation:
            fail("c-returns", f"operations[{index}] names a return but no C symbol")
        if "c_params" in operation:
            # The wire field names stay as they are -- the first is the scope key
            # by rule -- so the C signature carries its own names rather than
            # exporting "key" into a header that has always said repo_path.
            parameters = operation["c_params"]
            reply_shape = operation["reply"]
            request_shape = operation["request"]
            inbound = (int(request_shape["struct_from"]) + 1
                       if "struct_from" in request_shape
                       else 1 if "struct" in request_shape
                       else len(request_shape["fields"]))
            if "repeated" in request_shape:
                inbound += 2  # the values array and its count
            if operation.get("c_returns") == "text" and "scalars" in reply_shape:
                # The document is the return; the values beside it are ordinary
                # out-parameters and counted the ordinary way, minus the first
                # field, which is the document itself.
                outbound = sum(2 if str(f["type"]) == "text" and "alloc" not in f else 1
                               for f in reply_shape["fields"][1:])
            elif operation.get("c_returns") in ("text", "int64"):
                # The value comes back as the return, so there is no out
                # parameter to name -- the caller frees what it is handed, or
                # simply reads the id.
                outbound = 0
            elif "scalars" in reply_shape:
                # One pointer per numeric value; two per text one, because a
                # string out-parameter is a buffer AND the room in it. A reply
                # of loose scalars is not a row: there is no struct to put them
                # in, and a caller taking "char *role_out, size_t role_cap,
                # char *prompt_out, size_t prompt_cap" is asking for two
                # strings rather than a type.
                # -- unless the string is an allocation the caller frees, which
                # is one parameter and no capacity at all.
                outbound = sum(2 if str(f["type"]) == "text" and "alloc" not in f else 1
                               for f in reply_shape["fields"])
            elif "list" in reply_shape:
                # T *out, however wide the rows -- and one more when the count
                # comes back through a parameter instead of the return.
                outbound = 2 if "count" in reply_shape["list"] else 1
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
        # "struct_from" is the index where the struct's members begin, for a
        # domain that takes a key AND a row. Without it the struct is the whole
        # input, which is the ordinary case.
        if "struct_from" in operation["request"]:
            request_keys = request_keys | {"struct_from"}
        if "repeated" in operation["request"]:
            request_keys = request_keys | {"repeated"}
        request = keys(operation["request"], request_keys, f"{name}.request")
        if "struct" in request and not re.fullmatch(r"[a-z][a-z0-9_]*_t", str(request["struct"])):
            fail("request-struct", f"{name} request struct must be a _t type name")
        if "struct_from" in request:
            if "struct" not in request:
                fail("request-struct",
                     f"{name} says where its struct's members begin but declares no struct")
            integer(request["struct_from"], f"{name}.request.struct_from", 1, 64)
        raw_fields = request["fields"]
        # An operation may take nothing at all -- "what is the queue's status"
        # names no row. It must then say global out loud, because a scoped
        # operation is scoped BY its first field and there is no field to scope
        # by; declaring that is the audit trail.
        if not isinstance(raw_fields, list):
            fail("request-fields", f"{name} request fields must be an array")
        if not raw_fields and operation["scope"] != "global":
            fail("request-fields",
                 f"{name} takes no arguments, so it cannot be scoped: say global")
        fields = []
        for position, entry_field in enumerate(raw_fields):
            repeated_member = isinstance(entry_field, dict) and "repeat" in entry_field
            nested_member = isinstance(entry_field, dict) and "fields" in entry_field
            allowed = {"name", "type", "required"}
            if isinstance(entry_field, dict) and "null_when_empty" in entry_field:
                # The wire has no NULL: an absent string arrives as "". For most
                # parameters that is the same thing, and for some it is not --
                # db1_roundtable_run_list reads NULL as "every non-terminal run"
                # and "" as "state equals the empty string", which matches
                # nothing. Saying so here restores the distinction the C
                # signature always had.
                allowed = allowed | {"null_when_empty"}
            if repeated_member:
                allowed = allowed | {"repeat"}
            if nested_member:
                allowed = allowed | {"fields"}
            declared = keys(entry_field, allowed, f"{name}.request.fields[{position}]")
            if declared.get("null_when_empty") is not None:
                if declared["null_when_empty"] is not True:
                    fail("null-when-empty",
                         f"{name} field {declared['name']!r} null_when_empty must be true")
                if str(declared["type"]) != "text":
                    fail("null-when-empty",
                         f"{name} field {declared['name']!r} is NULL when empty, which is "
                         f"about a string")
                if declared.get("required"):
                    fail("null-when-empty",
                         f"{name} field {declared['name']!r} is required, so it is never "
                         f"empty and never NULL")
            if repeated_member:
                # Only a struct has members wide enough to need this. A bare
                # argument that repeats is the `repeated` shape, which carries
                # its own count and belongs at the end of the frame.
                if "struct" not in request:
                    fail("field-repeat",
                         f"{name} field {declared['name']!r} repeats, which only a struct "
                         f"member does; a repeating argument is the repeated shape")
                if str(declared["type"]) not in ("text", "struct"):
                    fail("field-repeat",
                         f"{name} field {declared['name']!r} repeats, so it carries text or "
                         f"rows: a repeated number has no caller yet and no test")
                integer(declared["repeat"], f"{name}.request.fields[{position}].repeat", 2, 64)
            if type(declared["required"]) is not bool:
                fail("field-required",
                     f"{name} field {declared['name']!r} required must be boolean")
            # A scope key that may be absent is not a scope key.
            if position == 0 and not declared["required"] and operation["scope"] != "global":
                fail("field-required",
                     f"{name} is scoped, so its first field cannot be optional")
            # A member that carries rows says "struct" and declares them; every
            # other field is one of the closed payload types.
            if declared["type"] not in FIELD_TYPES and not (nested_member and
                                                            str(declared["type"]) == "struct"):
                fail("field-type",
                     f"{name} field {declared['name']!r} type must be one of {list(FIELD_TYPES)}")
            fields.append(str(declared["name"]))
        raw_fields = expand_repeats(raw_fields)
        request["fields"] = raw_fields
        fields = [str(f["name"]) for f in raw_fields]
        operation["_field_types"] = [str(f["type"]) for f in raw_fields]
        if "repeated" in request:
            # A variable-length list of strings, carried at the END of the
            # frame. The operation's fixed fields come first, so the stage can
            # hand the domain a slice of its own decoded array rather than
            # copying: field[] is already const char *[], and &field[base] is
            # exactly the const char *const * the domain takes.
            # A repeated value is a string unless the operation says it is a
            # row: "struct" names the type and "fields" its members, and the
            # frame then carries one group of cells per element.
            allowed_rep = {"values", "count", "max_values"}
            if "struct" in request["repeated"]:
                allowed_rep |= {"struct", "fields"}
            # "kind" says what one element is when it is not a string: a list of
            # row ids crosses as numbers, and rendering them as text would be
            # the same bytes with a looser contract.
            if "kind" in request["repeated"]:
                allowed_rep |= {"kind"}
            rep = keys(request["repeated"], allowed_rep, f"{name}.request.repeated")
            if "kind" in rep:
                if str(rep["kind"]) not in ("int", "int64"):
                    fail("request-repeated",
                         f"{name} repeats {rep['kind']!r}; a repeated element is text unless "
                         f"it is a number")
                if "struct" in rep:
                    fail("request-repeated",
                         f"{name} repeats either a row or a bare value, not both")
            if "struct" in rep:
                if not re.fullmatch(r"[a-z][a-z0-9_]*_t", str(rep["struct"])):
                    fail("request-repeated",
                         f"{name} repeated struct must be a _t type name")
                if not isinstance(rep["fields"], list) or not rep["fields"]:
                    fail("request-repeated",
                         f"{name} repeats a row, so it declares that row's members")
                for position, member in enumerate(rep["fields"]):
                    shaped = keys(member, {"name", "type"},
                                  f"{name}.request.repeated.fields[{position}]")
                    if shaped["type"] not in FIELD_TYPES:
                        fail("request-repeated",
                             f"{name} repeated member {shaped['name']!r} type must be one "
                             f"of {list(FIELD_TYPES)}")
                    if not NAME.fullmatch(str(shaped["name"])):
                        fail("request-repeated",
                             f"{name} invalid repeated member {shaped['name']!r}")
            if "struct" in request:
                fail("request-repeated", f"{name} cannot repeat and take a struct")
            if "c_params" not in operation:
                fail("request-repeated", f"{name} repeats but names no C parameters")
            for role in ("values", "count"):
                if str(rep[role]) not in operation["c_params"]:
                    fail("request-repeated",
                         f"{name} repeated {role} {rep[role]!r} is not a C parameter")
            # A repeated STRING is a search's terms and stays small. A repeated
            # ROW is a bulk replace -- a provider's whole model list -- so it is
            # bounded by what the frame can hold rather than by that habit.
            # A repeated STRING is a search's terms and stays small. A row or a
            # list of ids is a bulk call, bounded by what the frame can hold
            # rather than by that habit.
            integer(rep["max_values"], f"{name}.request.repeated.max_values",
                    1, 1024 if ("struct" in rep or "kind" in rep) else 64)

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
        elif not fields:
            pass  # nothing to scope by; the global declaration above is the rule
        elif operation["scope"] == "global":
            if fields[0] == "key":
                fail("request-global",
                     f"{name} is declared global but takes a key; scope it instead")
        elif operation["scope"] != "none" and fields[0] != "key":
            fail("request-key", f"{name} is scoped, so it must take its key first")
        for field in fields:
            # An expanded array member is spelled the way C spells an element,
            # which is the point: every emitter writes it straight through.
            if not isinstance(field, str) or not ELEMENT.fullmatch(field):
                fail("request-field-name", f"{name} declares invalid request field {field!r}")
        if len(set(fields)) != len(fields):
            fail("request-field-duplicate", f"{name} repeats a request field")

        reply_keys = {"fields", "max_bytes"}
        if "out" in operation["reply"]:
            reply_keys = reply_keys | {"out"}
        if "scalars" in operation["reply"]:
            reply_keys = reply_keys | {"scalars"}
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
            # "allocate" says the callee owns the array as well as the rows:
            # the parameter is T ** and the caller frees what comes back. The
            # bound is still the caller's ceiling, not an allocation the wire
            # may be talked into.
            allowed_list = {"out", "bound", "max_rows"}
            if "allocate" in reply["list"]:
                allowed_list = allowed_list | {"allocate"}
            # "count" names a parameter that receives how many rows came back,
            # for a domain that returns 0/-1 instead. The ceiling is then
            # max_rows alone: there is no caller bound to clamp to.
            if "count" in reply["list"]:
                allowed_list = allowed_list | {"count"}
                if "bound" not in reply["list"]:
                    allowed_list = allowed_list - {"bound"}
            if "column" in reply["list"]:
                allowed_list = allowed_list | {"column"}
            listed = keys(reply["list"], allowed_list, f"{name}.reply.list")
            # A column is a list whose row is ONE value rather than a struct:
            # int64_t *out, or char (*out)[WIDTH]. Same frame, same arithmetic,
            # width one -- the only difference is that the row has no member to
            # name, so the catalog says what the row's C type is instead.
            if "column" in listed:
                column = keys(listed["column"], {"kind", "width"}
                              if "width" in listed["column"] else {"kind"},
                              f"{name}.reply.list.column")
                if "struct" in reply:
                    fail("reply-column", f"{name} is a column, so it declares no row struct")
                # Numeric columns are enabled now that windows has one to
                # prove: db1_windows_list_ids_by_tier_before_days answers with
                # int64_t *out_ids. Text and numeric take the same path; only
                # the row's declared C type differs.
                if (str(column["kind"]) == "text") != ("width" in column):
                    fail("reply-column",
                         f"{name} column of text needs a width, and a numeric one has none")
                if "width" in column and not re.fullmatch(r"[A-Z][A-Z0-9_]*", str(column["width"])):
                    fail("reply-column",
                         f"{name} column width must be a C identifier, not a literal: the row is "
                         f"as wide as the header says it is")
                # reply["fields"] rather than the reply_fields local, which is
                # not assigned until below: reading it here would validate the
                # PREVIOUS operation's reply and pass or fail for its reasons.
                declared_fields = reply["fields"]
                if not isinstance(declared_fields, list) or len(declared_fields) != 1:
                    fail("reply-column", f"{name} column declares exactly one value per row")
                if str(declared_fields[0]["type"]) != str(column["kind"]):
                    fail("reply-column",
                         f"{name} column kind {column['kind']!r} disagrees with its declared "
                         f"field type {declared_fields[0]['type']!r}")
            elif "struct" not in reply:
                fail("reply-list", f"{name} declares a list but no row struct")
            if "c_params" not in operation:
                fail("reply-list", f"{name} declares a list but names no C parameters")
            params = list(operation["c_params"])
            if str(listed["out"]) not in params:
                fail("reply-list", f"{name} list out {listed['out']!r} is not a C parameter")
            if "count" in listed:
                if not listed.get("allocate") and "bound" not in listed:
                    fail("reply-list",
                         f"{name} reports its count through a parameter and takes no bound, "
                         f"which only a list the callee allocates can do")
                if str(listed["count"]) not in params:
                    fail("reply-list",
                         f"{name} list count {listed['count']!r} is not a C parameter")
                if listed["count"] == listed["out"]:
                    fail("reply-list", f"{name} list out and count must differ")
            elif str(listed["bound"]) not in params:
                fail("reply-list", f"{name} list bound {listed['bound']!r} is not a C parameter")
            elif listed["out"] == listed["bound"]:
                fail("reply-list", f"{name} list out and bound must differ")
            # The remaining parameters map onto the request fields in order, so
            # the bound's position tells us which field must be the integer.
            excluded_names = {str(listed["out"])}
            if "count" in listed:
                excluded_names |= {str(listed["count"])}
            if "repeated" in request:
                excluded_names |= {str(request["repeated"]["values"]),
                                   str(request["repeated"]["count"])}
            inputs = [p for p in params if p not in excluded_names]
            if len(inputs) != len(raw_fields):
                fail("reply-list",
                     f"{name} has {len(inputs)} input parameters but {len(raw_fields)} "
                     f"request fields")
            if "count" not in listed:
                at = inputs.index(str(listed["bound"]))
                if str(raw_fields[at]["type"]) != "int":
                    fail("reply-list",
                         f"{name} list bound {listed['bound']!r} maps to request field "
                         f"{raw_fields[at]['name']!r}, which must be an int")
            if "allocate" in listed:
                if listed["allocate"] is not True:
                    fail("reply-list", f"{name} list allocate must be true when present")
                if "column" in listed:
                    fail("reply-list",
                         f"{name} allocates its rows, so it repeats a struct: a column has "
                         f"no row type to allocate")
            integer(listed["max_rows"], f"{name}.reply.list.max_rows", 1, 4096)
        reply_fields = reply["fields"]
        if not isinstance(reply_fields, list):
            fail("reply-fields", f"{name} reply fields must be an array")
        scalar_reply = "scalars" in operation["reply"]
        for position, declared in enumerate(reply_fields):
            # A text scalar is written into a stage-side buffer before it is
            # sent, so it says how wide that buffer is. Nothing else in a reply
            # needs one: a struct member is as wide as the struct says, and a
            # column already declares its own.
            nested_member = isinstance(declared, dict) and "fields" in declared
            if nested_member:
                if "struct" not in reply:
                    fail("field-nested",
                         f"{name} reply field {declared['name']!r} has members, which only a "
                         f"struct member does")
                if str(declared["type"]) != "struct":
                    fail("field-nested",
                         f"{name} reply field {declared['name']!r} has members, so it is "
                         f"declared struct rather than {declared['type']!r}")
                if not isinstance(declared["fields"], list) or not declared["fields"]:
                    fail("field-nested",
                         f"{name} nested member {declared['name']!r} declares no members")
            repeated_member = isinstance(declared, dict) and "repeat" in declared
            allowed_field = ({"name", "type", "fields", "repeat"}
                             if nested_member and "repeat" in declared
                             else {"name", "type", "fields"} if nested_member
                             else {"name", "type", "width"}
                             if scalar_reply and isinstance(declared, dict) and "width" in declared
                             else {"name", "type", "repeat"} if repeated_member
                             else {"name", "type"})
            allocated_member = isinstance(declared, dict) and "alloc" in declared
            if allocated_member:
                allowed_field = allowed_field | {"alloc"}
                # A struct member the store allocated, or a loose value handed
                # back through a char **. Both are memory the caller frees; the
                # difference is only where it is delivered.
                if "struct" not in reply and not scalar_reply:
                    fail("field-alloc",
                         f"{name} reply field {declared['name']!r} allocates, which is a "
                         f"struct member or a scalar and neither here")
                if str(declared["type"]) != "text":
                    fail("field-alloc",
                         f"{name} reply field {declared['name']!r} allocates, so it "
                         f"carries text")
                if "struct" in reply and str(declared["name"]) not in pointer_members(
                        root, str(reply["struct"])):
                    fail("field-alloc",
                         f"{name} reply field {declared['name']!r} allocates, but "
                         f"{reply['struct']} declares it inline: an inline array is already "
                         f"as long as it will ever be")
                integer(declared["alloc"], f"{name}.reply.fields[{position}].alloc", 1, 1 << 20)
            if repeated_member:
                if "struct" not in reply:
                    fail("field-repeat",
                         f"{name} reply field {declared['name']!r} repeats, which only a "
                         f"struct member does")
                # An array of strings, or an array of rows when it says what a
                # row is. Anything else repeating is a number nobody has needed.
                if str(declared["type"]) not in ("text", "struct"):
                    fail("field-repeat",
                         f"{name} reply field {declared['name']!r} repeats, so it carries text "
                         f"or rows")
                if str(declared["type"]) == "struct" and "fields" not in declared:
                    fail("field-repeat",
                         f"{name} reply field {declared['name']!r} repeats rows, so it says "
                         f"what a row is")
                integer(declared["repeat"], f"{name}.reply.fields[{position}].repeat", 2, 64)
            shape = keys(declared, allowed_field, f"{name}.reply.fields[{position}]")
            if nested_member:
                for at, member in enumerate(shape["fields"]):
                    # A row's own member may be an array: plan_step_t holds
                    # depends_on[AGENT_MAX_PLAN_DEPS], and a plan holds 32 of
                    # those steps. Declaring it is the only way to carry it, and
                    # carrying it is not optional -- the struct-members rule
                    # requires every member, so a step's dependencies cannot be
                    # quietly left behind on the far side of the wire.
                    inner_keys = ({"name", "type", "repeat"}
                                  if isinstance(member, dict) and "repeat" in member
                                  else {"name", "type"})
                    inner = keys(member, inner_keys,
                                 f"{name}.reply.fields[{position}].fields[{at}]")
                    if "repeat" in inner:
                        integer(inner["repeat"],
                                f"{name}.reply.fields[{position}].fields[{at}].repeat", 2, 64)
                    if inner["type"] not in PAYLOADS or inner["type"] == "none":
                        fail("field-nested",
                             f"{name} nested member {inner['name']!r} type must be a payload")
                continue
            if shape["type"] not in PAYLOADS or shape["type"] == "none":
                fail("reply-payload",
                     f"{name} reply field type must be one of {[p for p in PAYLOADS if p != 'none']}")
            if not NAME.fullmatch(str(shape["name"])):
                fail("reply-field-name", f"{name} invalid reply field {shape['name']!r}")
        # A row crosses whole or it does not cross honestly. The catalog says
        # which members travel, and a member it leaves out is not refused --
        # it is simply never written, so the caller reads the zero memset left
        # and cannot tell that from a row whose field really is empty.
        for shaped, where in ((reply, "reply"), (request, "request")):
            if "struct" not in shaped:
                continue
            # Fields before struct_from are ordinary arguments beside the
            # struct, not members of it.
            lead = int(shaped["struct_from"]) if "struct_from" in shaped else 0
            declared = [str(f["name"]).split("[")[0].split(".")[0]
                        for f in shaped["fields"][lead:]]
            seen, ordered = set(), []
            for one in declared:
                if one not in seen:
                    seen.add(one)
                    ordered.append(one)
            actual = struct_members(root, str(shaped["struct"]))
            if actual and ordered != actual:
                missing = [m for m in actual if m not in seen]
                extra = [m for m in ordered if m not in set(actual)]
                fail("struct-members",
                     f"{name} {where} declares {shaped['struct']} as {ordered}, but the "
                     f"header declares {actual}"
                     + (f"; missing {missing}" if missing else "")
                     + (f"; unknown {extra}" if extra else ""))
        reply_fields = expand_repeats(reply_fields)
        reply["fields"] = reply_fields
        operation["reply"]["fields"] = reply_fields
        if "scalars" in operation["reply"]:
            listed_fields = operation["reply"]["fields"]
            if operation["reply"]["scalars"] is not True:
                fail("reply-scalars", f"{name} scalars must be true when present")
            if "struct" in operation["reply"] or "list" in operation["reply"]:
                fail("reply-scalars", f"{name} is scalars, so it declares no struct or list")
            if not isinstance(listed_fields, list) or not listed_fields:
                fail("reply-scalars", f"{name} scalars must declare at least one value")
            for shape in listed_fields:
                if str(shape["type"]) not in NUMERIC + ("text",):
                    fail("reply-scalars",
                         f"{name} scalars carry numbers or text; {shape['name']!r} is "
                         f"{shape['type']!r}")
                if (str(shape["type"]) == "text") != ("width" in shape or "alloc" in shape):
                    fail("reply-scalars",
                         f"{name} scalar {shape['name']!r} declares a width or an alloc "
                         f"exactly when it carries text: the stage cannot see the caller's "
                         f"buffer, so the contract says how much it may produce")
                if "width" in shape and "alloc" in shape:
                    fail("reply-scalars",
                         f"{name} scalar {shape['name']!r} is delivered one way: into the "
                         f"caller's buffer, or as an allocation the caller frees")
                if "width" in shape and not re.fullmatch(r"[A-Z][A-Z0-9_]*", str(shape["width"])):
                    fail("reply-scalars",
                         f"{name} scalar width must be a C identifier, not a literal: the "
                         f"value is as wide as the header says it is")
        if operation.get("c_returns") == "found" and "missing" not in results:
            fail("c-returns",
                 f"{name} distinguishes found from nothing, so it must declare missing")
        if operation.get("c_returns") == "rc":
            if "struct" in reply or "list" in reply or "scalars" in reply:
                fail("c-returns",
                     f"{name} carries its return beside a read, so its reply is one value")
            if len(reply_fields) != 1 or str(reply_fields[0]["type"]) != "text":
                fail("c-returns",
                     f"{name} carries its return beside a read, so its reply declares "
                     f"exactly one text value: the buffer the caller passed")
        if "negatives" in operation:
            # "The number IS the answer, including when it is negative."
            #
            # An integer return is normally read as a status: the stage maps a
            # negative to FAILED, because for a count or an id a negative means
            # the store could not answer. db1_wfe_bind is not that -- it returns
            # -2 to say the work item is already bound to a DIFFERENT session,
            # which is the single-writer rule doing its job. Mapped as a status
            # that refusal arrives as -1 and reads as an outage.
            #
            # Same distinction ensemble draws between a verdict and a broken
            # store; ensemble had data to carry so it used a struct, and this
            # has only the number.
            if str(operation["negatives"]) != "data":
                fail("negatives", f"{name} negatives is 'data' or absent")
            if operation.get("c_returns") != "int64":
                fail("negatives",
                     f"{name} says its negatives are data, which is about an integer "
                     f"return: say c_returns int64")
        if operation.get("c_returns") == "void":
            # Nothing comes back as a RETURN. Out-parameters are a different
            # question: db1_clarify_weakest_dim fills a buffer and answers
            # nothing about whether it did, which is the contract it has.
            if reply_fields and "scalars" not in reply:
                fail("c-returns",
                     f"{name} returns nothing, so anything it carries is an out-parameter: "
                     f"say scalars, or carry nothing")
            if declared_return(root, str(operation["c_name"])) != "void":
                fail("c-returns",
                     f"{name} declares c_returns void, but its header does not")
        if operation.get("c_returns") == "member":
            if "struct" not in reply:
                fail("c-returns", f"{name} returns a member, so its reply is a struct")
            if "missing" not in results:
                fail("c-returns",
                     f"{name} returns a member, so it must declare missing: the member "
                     f"is how its callers ask whether there was anything there")
            member = text(operation["c_member"], f"{name}.c_member", 64)
            picked = [f for f in reply_fields if str(f["name"]) == member]
            if not picked:
                fail("c-returns", f"{name} c_member {member!r} is not a reply field")
            if str(picked[0]["type"]) not in NUMERIC:
                fail("c-returns",
                     f"{name} returns member {member!r}, which carries "
                     f"{picked[0]['type']!r} rather than a number")
        if operation.get("c_returns") == "int64":
            if "struct" in reply or "list" in reply or "scalars" in reply:
                fail("c-returns", f"{name} returns an id, so its reply is one value")
            # The reply's width follows the header, not a fixed choice here: a
            # domain that answers "how many rows changed" returns int, and
            # declaring int64 beside it would be a second, disagreeing
            # statement about the same function.
            spelled = declared_return(root, str(operation["c_name"]))
            # A cost is returned as a double, and rounding it at the boundary
            # would bill differently on each side of it.
            # An enum is an int with names on it, and the wire has no names --
            # the JTI replay checks answer ok / replay / saturated / storage /
            # invalid, and the caller admits a request on the first and refuses
            # it on the rest, so the VALUE has to survive. It travels as an int
            # and the client casts it back to the declared type, which is what
            # keeps "storage failed" distinguishable from "this is a replay".
            enum_return = spelled.endswith("_result_t")
            wanted = (("double",) if spelled == "double"
                      else ("float",) if spelled == "float"
                      else ("int", "int64") if spelled in ("int", "long") or enum_return
                      else ("int64",))
            if len(reply_fields) != 1 or str(reply_fields[0]["type"]) not in wanted:
                fail("c-returns",
                     f"{name} hands back its return value, so its reply must declare "
                     f"exactly one of {list(wanted)}: its header returns {spelled}")
        if operation.get("c_returns") == "text" and "scalars" in reply:
            # A cached page is the document AND how old it is: the return is the
            # first value, and the rest are ordinary out-parameters beside it.
            if str(reply_fields[0]["type"]) != "text":
                fail("c-returns",
                     f"{name} returns a string, so the first value it declares is that "
                     f"string; the ones after it are the out-parameters beside it")
        elif operation.get("c_returns") == "text":
            # The reply is the return value, so there is exactly one of it and
            # its declared size is what the client allocates before it calls.
            if "struct" in reply or "list" in reply:
                fail("c-returns", f"{name} returns a string, so its reply is one value")
            if len(reply_fields) != 1 or str(reply_fields[0]["type"]) != "text":
                fail("c-returns",
                     f"{name} returns a string, so its reply must declare exactly one "
                     f"text field")
        max_bytes = integer(reply["max_bytes"], f"{name}.reply.max_bytes", 0, 1 << 20)
        if (not reply_fields) != (max_bytes == 0):
            fail("reply-bytes", f"{name} reply max_bytes must be zero exactly when it carries nothing")
        operations.append(operation)
    return operations


def validate_catalog(value: object, root: Path) -> dict[str, object]:
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
    operations = validate_operations(catalog["operations"], families, root)
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
            if "text" in kinds and operation.get("c_returns") != "text":
                field_max = max(field_max, int(reply["max_bytes"]))
            if operation["wire_format"] == "db1-fields-v2":
                widest_request = len(request["fields"])
                if "repeated" in request:
                    widest_request += int(request["repeated"]["max_values"])
                fields_max = max(fields_max, widest_request)

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
   /* Zero fields is a legal request: an operation that takes no arguments
      sends the header alone. The upper bound still applies. */
   if (count > AIMEE_DB1_FIELDS_MAX)
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
      /* Fewer values than the caller has slots for is the same contract
         mismatch read from the other side, and it used to pass: the unfilled
         slots keep the empty string cleared above, so the caller reads a row
         whose last members are blank and cannot tell that from a row that is
         blank. A list says how many rows it found through filled_out and is
         variable by construction; every other shape has one arity, and a stage
         answering with a different one is a stage built against a different
         version of this contract. Two processes, two binaries, two deployment
         times -- so say it rather than zero-fill. */
      else if (status == (uint32_t)AIMEE_DB1_STATUS_OK && fields_in != slots)
         result = -1;
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

{write_result}{read_result}"""


WRITE_RESULT = """/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}

"""


READ_RESULT = """/* A read answers found(1) / not-found(0) / error(-1), which is what the direct
   implementation returns and what its callers already branch on. */
static int read_result(int status, const char *value_out)
{
   if (status == (int)AIMEE_DB1_STATUS_OK)
      return (value_out && value_out[0]) ? 1 : 0;
   if (status == (int)AIMEE_DB1_STATUS_MISSING)
      return 0;
   return -1;
}
"""


# Header text, read and comment-stripped once per root.
#
# Four helpers below ask the headers a question per operation: what a symbol
# returns, how it spells its parameters, which struct members are pointers, and
# which header declares what. Each used to glob the directory, read every file
# and strip its comments for every question asked. At 275 operations over ~70
# headers that is fifty thousand regex searches and five seconds a run -- times
# fifty-one tests, which is what pushed the descriptor-envelope job past its
# five-minute budget and got it cancelled rather than failed.
#
# Keyed by root because the tests build catalogs in sandboxes and a cache that
# ignored the root would answer one sandbox's question with another's headers.
_HEADER_CACHE: dict[str, list[tuple[str, str, str]]] = {}


def header_texts(root: Path) -> list[tuple[str, str, str]]:
    """(name, raw, comment-stripped) for every DB1 header, in sorted order."""
    key = str(root)
    cached = _HEADER_CACHE.get(key)
    if cached is None:
        cached = []
        for header in sorted((root / SOURCE_DIR).glob("*.h")):
            raw = header.read_text(errors="ignore")
            cached.append((header.name, raw, re.sub(r"/\*.*?\*/", "", raw, flags=re.S)))
        _HEADER_CACHE[key] = cached
    return cached


def forget_header_texts() -> None:
    """Drop the caches, for a caller that rewrites headers between runs."""
    _HEADER_CACHE.clear()
    _SYMBOL_CACHE.clear()
    _WIDER_CACHE.clear()


_SYMBOL_CACHE: dict[str, dict[str, tuple[str, str, str]]] = {}


def declaring_headers(root: Path) -> dict[str, tuple[str, str, str]]:
    """Which header names each symbol, so a lookup reads one file rather than all.

    Scanning every header for every symbol is the same answer and quadratic
    work: the regex that finds a return type backtracks across a whole file to
    conclude the symbol is not in it, and it concludes that about sixty-nine
    files out of seventy.
    """
    key = str(root)
    index = _SYMBOL_CACHE.get(key)
    if index is None:
        index = {}
        called = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
        for entry in header_texts(root):
            for symbol in called.findall(entry[2]):
                index.setdefault(symbol, entry)
        _SYMBOL_CACHE[key] = index
    return index


def domain_headers(root: Path, operations: list[dict[str, object]]) -> list[str]:
    """The DB1 headers that declare these operations.

    Derived rather than declared: the family name and the source name coincided
    for the first family and do not in general, so a header named after the
    family is a header that does not exist.
    """
    index = declaring_headers(root)
    found = {index[symbol][0]
             for symbol in (str(o.get("c_name", "")) for o in operations)
             if symbol and symbol in index}
    return sorted(found)


def declared_parameters(root: Path, symbol: str) -> dict[str, str]:
    """Parameter name -> the type the header spells for it.

    The generator used to derive a C type from the catalog's FIELD type: text
    became const char *, int became int. That holds until the domain spells it
    differently -- an enum passed by value is an int on the wire and
    db1_user_recall_section_t in the signature -- and "int section" declares a
    different function. Same lesson as the return type: read the header.
    """
    pattern = re.compile(r"\b" + re.escape(symbol) + r"\s*\(([^;{]*)\)\s*;", re.S)
    entry = declaring_headers(root).get(symbol)
    for _name, _raw, stripped in ([entry] if entry else []):
        found = pattern.search(stripped)
        if not found:
            continue
        spelled: dict[str, str] = {}
        for piece in found.group(1).split(","):
            piece = " ".join(piece.split())
            named = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*\])?$", piece)
            if named:
                spelled[named.group(1)] = piece
        return spelled
    return {}


def declared_return(root: Path, symbol: str) -> str:
    """The return type the header actually spells for `symbol`.

    int64_t and long long are the same width and NOT the same type to the
    compiler: on this platform int64_t is long int, so generating one where the
    header says the other is a conflicting declaration. The header is the
    contract; this reads it rather than assuming a spelling.
    """
    entry = declaring_headers(root).get(symbol)
    if entry:
        pattern = re.compile(r"([A-Za-z_][A-Za-z0-9_ ]*?)\s+" + re.escape(symbol) + r"\s*\(")
        found = pattern.search(entry[2])
        if found:
            return " ".join(found.group(1).split())
    return "int64_t"


_WIDER_CACHE: dict[str, list[tuple[str, str, str]]] = {}


def wider_headers(root: Path) -> list[tuple[str, str, str]]:
    """Headers outside src/modules/db1 that declare a type the wire carries.

    cron_job_t lives in the config module and provider_model_t in src/headers,
    and both cross the boundary. Looking only where DB1 keeps its own headers
    would leave exactly those two rows unchecked -- and a rule that silently
    has no opinion about some of the rows it is meant to guard is worse than
    one that says so.
    """
    key = str(root)
    cached = _WIDER_CACHE.get(key)
    if cached is None:
        cached = []
        seen: set[Path] = set()
        for pattern in ("headers/*.h", "modules/*/*.h", "modules/*/include/*.h",
                        "modules/*/include/*/*.h"):
            for header in sorted((root / "src").glob(pattern)):
                if header in seen:
                    continue
                seen.add(header)
                raw = header.read_text(errors="ignore")
                cached.append((header.name, raw, re.sub(r"/\*.*?\*/", "", raw, flags=re.S)))
        _WIDER_CACHE[key] = cached
    return cached


def struct_body(text: str, struct: str) -> str | None:
    """The body of `typedef struct { ... } struct;`, matched by braces.

    A regex cannot do this: ".*?" between the first "typedef struct {" and the
    named closing brace swallows every struct declared before it in the same
    header, and the members of those come back as members of this one. Walking
    the braces is the only way to know which "}" belongs to which "{".
    """
    for opened in re.finditer(r"typedef\s+struct\s*(?:[A-Za-z_][A-Za-z0-9_]*\s*)?\{", text):
        depth, at = 1, opened.end()
        while depth and at < len(text):
            depth += (text[at] == "{") - (text[at] == "}")
            at += 1
        tail = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*;", text[at:])
        if tail and tail.group(1) == struct:
            return text[opened.end():at - 1]
    return None


def struct_members(root: Path, struct: str) -> list[str]:
    """Every member `struct` declares, in order, or [] if it is not a DB1 type.

    The catalog says which members of a row cross the wire, and nothing used to
    check that against the row itself. A member the catalog omits is simply not
    carried: the client leaves it as memset left it, and the caller reads an
    empty string or a zero where a real value used to be. That is silent, it
    survives regeneration, and it is exactly what happens when somebody adds a
    field to a struct and does not think about the wire.

    Structs the wire does not own are not this rule's business -- a caller's own
    type is found here only if a DB1 header declares it -- so an empty list
    means "no opinion", not "no members".
    """
    for _name, _raw, stripped in header_texts(root) + wider_headers(root):
        body = struct_body(stripped, struct)
        if body is None:
            continue
        names: list[str] = []
        for member in body.split(";"):
            member = re.sub(r"/\*.*?\*/", "", member, flags=re.S).strip()
            if not member:
                continue
            # The declarator's name: the last identifier before any array
            # bounds, with the pointer star and the type in front of it.
            matched = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]\s*)*$", member)
            if matched:
                names.append(matched.group(1))
        return names
    return []


def pointer_members(root: Path, struct: str) -> set[str]:
    """Members of `struct` that are pointers rather than inline arrays.

    Only a pointer can be NULL. A char[N] member always has an address, so
    "member ? member : \"\"" on one is a comparison the compiler knows is
    always true -- a -Werror=address failure, and a tell that the question was
    the wrong one. An array member CAN still be empty, which is why this is
    separate from whether the field is required.
    """
    # struct_body rather than a regex: the non-greedy form this used matched from
    # the FIRST "typedef struct {" in the file to this struct's closing name, so
    # for a header holding two structs it returned the wrong one's members --
    # silently, and spelled the same way, which is why it read as correct.
    body = ""
    for _name, raw, _stripped in header_texts(root):
        found = struct_body(raw, struct)
        if found is not None:
            body = found
            break
    names: set[str] = set()
    for member in body.split(";"):
        member = re.sub(r"/\*.*?\*/", "", member, flags=re.S).strip()
        matched = re.search(r"\*\s*([A-Za-z_][A-Za-z0-9_]*)$", member)
        if matched:
            names.add(matched.group(1))
    # A member that is itself a struct contributes its own pointer members under
    # a dotted name, because that is how the expansion spells them: a request
    # row holding a token holds "token.jti", and asking this set about the outer
    # struct alone would call that an inline array and try to snprintf into a
    # const char *.
    for member in body.split(";"):
        member = re.sub(r"/\*.*?\*/", "", member, flags=re.S).strip()
        nested = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*_t)\s+([A-Za-z_][A-Za-z0-9_]*)", member)
        if not nested or nested.group(1) == struct:
            continue
        for inner in pointer_members(root, nested.group(1)):
            names.add(f"{nested.group(2)}.{inner}")
    return names


def client_bytes(catalog: dict[str, object], family: dict[str, object],
                 operations: list[dict[str, object]], headers: list[str],
                 root: Path) -> str:
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
    # Both helpers are emitted only if the generated code calls one, because an
    # unused static is a -Werror failure and the build stops.
    #
    # This used to be a predicate describing which shapes reach each helper, and
    # it had to be extended for every shape added -- list, column, returned
    # string, scalars, returned id -- each time discovered as a broken build
    # rather than as a wrong answer, because the compiler happened to notice.
    # ensemble was the next one: the first family whose every operation reads
    # something back, so write_result had no caller.
    #
    # Asking the emitted text which helpers it calls cannot fall behind a shape,
    # since a shape that does not call one is exactly a shape that does not need
    # it. The bodies are therefore built before the scaffold that precedes them.
    out = []
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
        repeated = request.get("repeated")
        scalars = reply.get("scalars")
        beside_return = (operation.get("c_returns") == "text") and bool(reply.get("scalars"))
        scalar_members = ([(str(f["name"]), str(f["type"]))
                           for f in reply["fields"][1 if beside_return else 0:]]
                          if scalars else [])
        scalar_alloc = ({str(f["name"]): int(f["alloc"]) for f in reply["fields"]
                         if "alloc" in f} if scalars else {})
        returns_text = operation.get("c_returns") == "text"
        returns_id = operation.get("c_returns") == "int64"
        returns_text_with_scalars = returns_text and bool(scalars)
        struct_inputs = (int(request["struct_from"]) + 1 if "struct_from" in request
                         else 1) if in_struct else 0
        if returns_text_with_scalars:
            split = struct_inputs or len(fields)
            inputs, outputs = names[:split], names[split:]
        elif returns_text or returns_id:
            inputs, outputs = list(names), []
        elif scalars:
            split = struct_inputs or len(fields)
            inputs, outputs = names[:split], names[split:]
        elif listed:
            # The rows parameter can sit anywhere in the signature -- some
            # domains put it first -- so the C order is read from c_params and
            # only the remaining parameters map onto the fields, in order.
            row_out = str(listed["out"])
            counted = str(listed["count"]) if "count" in listed else ""
            # With no caller bound, the ceiling is the one the catalog declares
            # -- the same number the stage refuses to exceed.
            bound = str(listed["bound"]) if "bound" in listed else str(listed["max_rows"])
            column = listed.get("column")
            excluded = {row_out} | ({counted} if counted else set())
            if repeated:
                excluded |= {str(repeated["values"]), str(repeated["count"])}
            inputs = [p for p in names if p not in excluded]
            outputs = [row_out]
        elif out_struct and "out" in reply:
            # Some domains put the out parameter FIRST. Assuming it is last put
            # the arguments in the wrong order with types that happened to
            # compile as a different function.
            row_out = str(reply["out"])
            inputs = [p for p in names if p != row_out]
            outputs = [row_out]
        else:
            lead = int(request["struct_from"]) if "struct_from" in request else 0
            split = (lead + 1) if in_struct else len(fields)
            inputs, outputs = names[:split], names[split:]

        if in_struct:
            # The struct is the argument; its members are the frame. Anything
            # before it is an ordinary argument that keeps its own type.
            lead = int(request["struct_from"]) if "struct_from" in request else 0
            params = [(f"int {p}" if t == "int"
                       else f"int64_t {p}" if t == "int64"
                       else f"double {p}" if t == "double"
                       else f"const char *{p}")
                      for p, t in zip(inputs[:lead], types[:lead])]
            params.append(f"const {in_struct} *{inputs[lead]}")
            guards = [f"!{p} || !{p}[0]" for p, t, need in
                      zip(inputs[:lead], types[:lead], required[:lead]) if t == "text" and need]
            guards.append(f"!{inputs[lead]}")
        else:
            # int64 is its own C type here. It only ever appeared as a struct
            # member before, where the struct's own declaration carried the
            # width; as a direct parameter, "int" is a different function.
            spelled = declared_parameters(root, str(operation.get("c_name", "")))
            params = [spelled.get(p) or (f"int {p}" if t == "int"
                                         else f"int64_t {p}" if t == "int64"
                                         else f"double {p}" if t == "double"
                                         else f"const char *{p}")
                      for p, t in zip(inputs, types)]
            # An int cannot be null and has no empty case, so only text is
            # guarded -- and only where the operation says it must be there.
            guards = [f"!{p} || !{p}[0]" for p, t, need in zip(inputs, types, required)
                      if t == "text" and need]
        if repeated:
            # The array and its count are declared where c_params puts them.
            declared_rep = dict(zip(inputs, params))
            declared_rep[str(repeated["values"])] = (
                f"const {repeated['struct']} *{repeated['values']}" if "struct" in repeated
                else f"const {'int' if repeated['kind'] == 'int' else 'int64_t'} "
                     f"*{repeated['values']}" if "kind" in repeated
                else f"const char *const *{repeated['values']}")
            declared_rep[str(repeated["count"])] = f"int {repeated['count']}"
            if listed:
                declared_rep[row_out] = f"{out_struct} *{row_out}"
            params = [declared_rep[p] for p in names]
            guards += [f"!{repeated['values']}", f"{repeated['count']} <= 0",
                       f"{repeated['count']} > {repeated['max_values']}"]
            if listed:
                guards += [f"!{row_out}", f"{bound} <= 0"]
        elif returns_text and not scalars:
            pass  # the value is the return; there is no out parameter
        elif returns_id:
            pass
        elif scalars or returns_text_with_scalars:
            # One parameter per numeric value, two per text one. The returned
            # string is not among them: it is the return.
            position = 0
            for member, kind in scalar_members:
                if kind == "text" and member in scalar_alloc:
                    # The value is an allocation the caller frees, so there is
                    # no capacity to pass: one parameter, one indirection more.
                    params.append(f"char **{outputs[position]}")
                    guards.append(f"!{outputs[position]}")
                    position += 1
                elif kind == "text":
                    buffer_name, cap_name = outputs[position], outputs[position + 1]
                    params.append(f"char *{buffer_name}")
                    params.append(f"size_t {cap_name}")
                    guards += [f"!{buffer_name}", f"{cap_name} == 0"]
                    position += 2
                else:
                    # The header's spelling, for the same reason an input takes
                    # it: long long and int64_t are the same width and not the
                    # same type, and declaring one where the header says the
                    # other is a conflicting declaration.
                    ctype = {"int": "int", "int64": "int64_t", "double": "double", "float": "float"}[kind]
                    out_spelled = declared_parameters(
                        root, str(operation.get("c_name", ""))).get(outputs[position])
                    params.append(out_spelled or f"{ctype} *{outputs[position]}")
                    guards.append(f"!{outputs[position]}")
                    position += 1
        elif listed:
            # Re-order to the C signature: the declarations above are in field
            # order, which is the same order minus the rows parameter.
            declared = dict(zip(inputs, params))
            if column:
                # char (*out)[WIDTH] for a text column, int64_t *out for a
                # numeric one: the row's own C type, spelled as the header does.
                declared[row_out] = (
                    f"char (*{row_out})[{column['width']}]" if str(column["kind"]) == "text"
                    else f"{'int64_t' if column['kind'] == 'int64' else 'int'} *{row_out}")
            else:
                declared[row_out] = (f"{out_struct} **{row_out}" if listed.get("allocate")
                                     else f"{out_struct} *{row_out}")
            if counted:
                declared[counted] = f"int *{counted}"
            params = [declared[p] for p in names]
            guards += [f"!{row_out}"] + ([f"!{counted}"] if counted
                                         else [f"{bound} <= 0"])
        elif out_struct and "out" in reply:
            declared_params = dict(zip(inputs, params))
            declared_params[str(reply["out"])] = f"{out_struct} *{reply['out']}"
            params = [declared_params[p] for p in names]
            guards += [f"!{reply['out']}"]
        elif out_struct:
            params += [f"{out_struct} *{outputs[0]}"]
            guards += [f"!{outputs[0]}"]
        elif reads:
            params += [f"char *{outputs[0]}", f"size_t {outputs[1]}"]
            guards += [f"!{outputs[0]}", f"{outputs[1]} == 0"]
        returns_void = operation.get("c_returns") == "void"
        kind = ("char *" if returns_text
                else "void " if returns_void
                else f"{declared_return(root, str(operation['c_name']))} " if returns_id
                else "int ")
        signature = f"{kind}{operation['c_name']}({', '.join(params)})"
        body = [signature, "{"]
        empty = "NULL" if returns_text else "" if returns_void else "-1"
        if guards:
            body += [f"   if ({' || '.join(guards)})",
                     f"      return{' ' + empty if empty else ''};"]
        if listed and "bound" in listed:
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
        if in_struct:
            lead = int(request["struct_from"]) if "struct_from" in request else 0
            sources = list(inputs[:lead]) + [f"{inputs[lead]}->{f}" for f in fields[lead:]]
        else:
            sources = list(inputs)
        # Only a pointer member can be NULL; an inline array always has an
        # address. None means "these are bare arguments", which always can be.
        member_pointers = pointer_members(root, in_struct) if in_struct else None
        for position, (source, kind, need) in enumerate(zip(sources, types, required)):
            local = f"arg{position}"
            nullable = (member_pointers is None or source.split("->")[-1] in member_pointers)
            if kind == "text" and not need and nullable:
                # The domains already read NULL as empty; the wire says so too
                # rather than refusing a caller that leaves a value out. This
                # applies to a struct's members as much as to a bare argument:
                # a row with a nullable "error" is the ordinary case, and
                # excluding struct members here refused those calls outright.
                carried.append(f"{source} ? {source} : \"\"")
                continue
            if kind in NUMERIC:
                spec, cast = numeric_format(kind)
                body.append(f"   char {local}[{NUMERIC_TEXT}];")
                body.append(f'   snprintf({local}, sizeof {local}, "{spec}", {cast}{source});')
                carried.append(local)
            else:
                carried.append(source)
        renders_numbers = False
        if repeated:
            # Fixed fields first, then the values. The stage recovers the count
            # by subtracting its own known arity from the frame's, so nothing
            # extra is sent to say how many there are.
            base = len(carried)
            row_members = [(str(f["name"]), str(f["type"]))
                           for f in repeated.get("fields", [])]
            span = len(row_members) or 1
            total = base + int(repeated["max_values"]) * span
            body.append(f"   const char *fields[{total}];")
            for index, value in enumerate(carried):
                body.append(f"   fields[{index}] = {value};")
            if row_members:
                # One group of cells per element. The numeric members need
                # somewhere to be rendered that outlives the loop, so they get
                # one buffer per element rather than one reused buffer.
                numeric_members = [i for i, (_, k) in enumerate(row_members) if k in NUMERIC]
                if numeric_members:
                    renders_numbers = True
                    body.append(f"   char (*wire_rendered)[{NUMERIC_TEXT}] = "
                                f"malloc((size_t){repeated['count']} * "
                                f"{len(numeric_members)}u * sizeof *wire_rendered);")
                    body.append("   if (!wire_rendered)")
                    body.append("      return -1;")
                body.append(f"   for (int at = 0; at < {repeated['count']}; ++at)")
                body.append("   {")
                slot = 0
                for index, (member, kind) in enumerate(row_members):
                    cell = f"{base} + at * {span} + {index}"
                    if kind in NUMERIC:
                        spec, cast = numeric_format(kind)
                        rendered = f"wire_rendered[at * {len(numeric_members)}u + {slot}u]"
                        body.append(f"      snprintf({rendered}, {NUMERIC_TEXT}, \"{spec}\", "
                                    f"{cast}{repeated['values']}[at].{member});")
                        body.append(f"      fields[{cell}] = {rendered};")
                        slot += 1
                    else:
                        body.append(f"      fields[{cell}] = {repeated['values']}[at].{member};")
                body.append("   }")
            elif "kind" in repeated:
                # One rendered number per element, in storage that outlives the
                # loop: the frame holds pointers, not copies.
                renders_numbers = True
                body.append(f"   char (*wire_rendered)[{NUMERIC_TEXT}] = "
                            f"malloc((size_t){repeated['count']} * sizeof *wire_rendered);")
                body.append("   if (!wire_rendered)")
                body.append("      return -1;")
                spec, cast = numeric_format(str(repeated["kind"]))
                body.append(f"   for (int at = 0; at < {repeated['count']}; ++at)")
                body.append("   {")
                body.append(f"      snprintf(wire_rendered[at], {NUMERIC_TEXT}, \"{spec}\", "
                            f"{cast}{repeated['values']}[at]);")
                body.append(f"      fields[{base} + at] = wire_rendered[at];")
                body.append("   }")
            else:
                body.append(f"   for (int at = 0; at < {repeated['count']}; ++at)")
                body.append(f"      fields[{base} + at] = {repeated['values']}[at] "
                            f"? {repeated['values']}[at] : \"\";")
        elif carried:
            body.append(f"   const char *fields[] = {{{', '.join(carried)}}};")
        else:
            # A zero-length array is not valid C; the frame carries no fields.
            body.append("   const char *const *fields = NULL;")
        op_symbol = f"AIMEE_DB1_OP_{str(operation['name']).upper()}"
        repeated_span = len([f for f in repeated.get("fields", [])]) if repeated else 0
        arity = ((f"(uint32_t)({len(fields)} + {repeated['count']} * {repeated_span})"
                  if repeated_span
                  else f"(uint32_t)({len(fields)} + {repeated['count']})") if repeated
                 else str(len(fields)))
        if returns_id:
            # The id IS the answer, so it crosses as a value and comes back as
            # the return. Flattening it to 0/-1 would tell the caller the row
            # was written without saying which row.
            body.append(f"   char slot0[{NUMERIC_TEXT}];")
            body.append("   char *const values[] = {slot0};")
            body.append("   const size_t caps[] = {sizeof slot0};")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, values, caps, "
                        "1, NULL);")
            body.append("   if (wire_status != (int)AIMEE_DB1_STATUS_OK)")
            body.append("      return -1;")
            spelled = declared_return(root, str(operation["c_name"])).strip()
            body.append(f"   return ({spelled})" +
                        ("strtod(slot0, NULL);" if spelled in ("double", "float")
                         else "strtoll(slot0, NULL, 10);"))
        elif returns_text_with_scalars:
            cap = int(reply["fields"][0].get("alloc", reply["max_bytes"]))
            body.append(f"   char *value = malloc({cap}u);")
            body.append("   if (!value)")
            body.append("      return NULL;")
            body.append("   value[0] = '\\0';")
            slots, caps, converts, position = ["value"], [f"{cap}u"], [], 0
            for index, (member, kind) in enumerate(scalar_members, start=1):
                if kind == "text":
                    slots.append(outputs[position])
                    caps.append(outputs[position + 1])
                    position += 2
                else:
                    body.append(f"   char slot{index}[{NUMERIC_TEXT}];")
                    slots.append(f"slot{index}")
                    caps.append(f"sizeof slot{index}")
                    converts.append((outputs[position], kind, f"slot{index}"))
                    position += 1
            body.append(f"   char *const values[] = {{{', '.join(slots)}}};")
            body.append(f"   const size_t caps[] = {{{', '.join(caps)}}};")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, values, "
                        f"caps, {len(slots)}, NULL);")
            # The document is the answer; an empty one is the miss the domain
            # signalled with NULL, and the values beside it are only meaningful
            # when there was something to be beside.
            body.append("   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])")
            body.append("   {")
            body.append("      free(value);")
            body.append("      return NULL;")
            body.append("   }")
            for out_name, kind, slot_name in converts:
                body.append(f"   *{out_name} = {numeric_parse(kind, slot_name)};")
            body.append("   char *shrunk = realloc(value, strlen(value) + 1u);")
            body.append("   return shrunk ? shrunk : value;")
        elif scalars:
            # Each value arrives as decimal text and converts into the caller's
            # own variable, and only once the whole reply is known good: a
            # partial write would leave the caller holding some new numbers and
            # some old ones with no way to tell which.
            # A text value is written straight into the caller's buffer; a
            # numeric one lands in a slot and converts. Both are filled only
            # once the whole reply is known good.
            slots, caps, converts, position = [], [], [], 0
            handed = []
            for index, (member, kind) in enumerate(scalar_members):
                if kind == "text" and member in scalar_alloc:
                    # Allocated here at the declared ceiling, shrunk to what
                    # arrived, and handed over only once the whole reply is
                    # good -- so a failure leaves the caller's pointer as it
                    # found it rather than owning a half-filled string.
                    ceiling = scalar_alloc[member]
                    body.append(f"   char *held{index} = malloc({ceiling}u);")
                    body.append(f"   if (!held{index})")
                    body.append("   {")
                    body += [f"      free(held{i});" for i, _ in handed]
                    body.append("      return -1;")
                    body.append("   }")
                    body.append(f"   held{index}[0] = '\\0';")
                    slots.append(f"held{index}")
                    caps.append(f"{ceiling}u")
                    handed.append((index, outputs[position]))
                    position += 1
                elif kind == "text":
                    slots.append(outputs[position])
                    caps.append(outputs[position + 1])
                    position += 2
                else:
                    body.append(f"   char slot{index}[{NUMERIC_TEXT}];")
                    slots.append(f"slot{index}")
                    caps.append(f"sizeof slot{index}")
                    converts.append((outputs[position], kind, f"slot{index}"))
                    position += 1
            body.append(f"   char *const values[] = {{{', '.join(slots)}}};")
            body.append(f"   const size_t caps[] = {{{', '.join(caps)}}};")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, values, caps, "
                        f"{len(scalar_members)}, NULL);")
            found = operation.get("c_returns") == "found"
            voided = operation.get("c_returns") == "void"
            body.append("   if (wire_status != (int)AIMEE_DB1_STATUS_OK)")
            body.append("   {")
            body += [f"      free(held{i});" for i, _ in handed]
            if voided:
                # The caller cannot be told, so it is left with the buffer it
                # arrived with rather than a half-written one.
                body.append("      return;")
            elif found:
                # Nothing there and broken are different answers, and a caller
                # polling for a live turn treats them differently.
                body.append("      return wire_status == (int)AIMEE_DB1_STATUS_MISSING "
                            "? 0 : -1;")
            else:
                body.append("      return -1;")
            body.append("   }")
            for out_name, kind, slot_name in converts:
                body.append(f"   *{out_name} = {numeric_parse(kind, slot_name)};")
            for index, out_name in handed:
                body.append(f"   char *shrunk{index} = realloc(held{index}, "
                            f"strlen(held{index}) + 1u);")
                body.append(f"   *{out_name} = shrunk{index} ? shrunk{index} : held{index};")
            if not voided:
                body.append("   return 1;" if found else "   return 0;")
        elif returns_text:
            cap = int(reply["max_bytes"])
            # Allocated at the declared maximum because the size is not known
            # until the reply arrives, then shrunk to what came back. The
            # caller frees it, which is the contract the domain already had --
            # the memory simply comes from here now.
            body.append(f"   char *value = malloc({cap}u);")
            body.append("   if (!value)")
            body.append("      return NULL;")
            body.append("   char *const values[] = {value};")
            body.append(f"   const size_t caps[] = {{{cap}u}};")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, values, caps, "
                        "1, NULL);")
            # An empty value is the miss the domain signalled with NULL, and the
            # two must stay the same answer: a caller that treats "" as content
            # would render an empty context rather than skipping it.
            body.append("   if (wire_status != (int)AIMEE_DB1_STATUS_OK || !value[0])")
            body.append("   {")
            body.append("      free(value);")
            body.append("      return NULL;")
            body.append("   }")
            body.append("   char *shrunk = realloc(value, strlen(value) + 1u);")
            body.append("   return shrunk ? shrunk : value;")
        elif listed:
            members = [(str(f["name"]), str(f["type"])) for f in reply["fields"]]
            width = len(members)
            numeric = [i for i, (_, k) in enumerate(members) if k in NUMERIC]
            allocates = bool(listed.get("allocate"))
            if allocates:
                # The callee owns the array as well as the rows. It is filled
                # into storage this side allocated and handed over only once
                # the whole reply is known good: a caller given a half-filled
                # array has no way to tell which half.
                body.append(f"   {out_struct} *wire_held = calloc((size_t){bound}, "
                            "sizeof *wire_held);")
                body.append("   if (!wire_held)")
                body.append("      return -1;")
                row_out = "wire_held"
            body.append(f"   char **wire_values = malloc((size_t){bound} * {width}u * sizeof *wire_values);")
            body.append(f"   size_t *wire_caps = malloc((size_t){bound} * {width}u * sizeof *wire_caps);")
            if numeric:
                body.append(f"   char (*wire_scratch)[{NUMERIC_TEXT}] = malloc((size_t){bound} * {len(numeric)}u * "
                            "sizeof *wire_scratch);")
            owned = "wire_values, wire_caps" + (", wire_scratch" if numeric else "")
            checks = " || ".join(f"!{o}" for o in owned.split(", "))
            body.append(f"   if ({checks})")
            body.append("   {")
            for item in owned.split(", "):
                body.append(f"      free({item});")
            if allocates:
                body.append("      free(wire_held);")
            body.append("      return -1;")
            body.append("   }")
            row_allocated = {str(f["name"]): int(f["alloc"])
                             for f in reply["fields"] if "alloc" in f}
            body.append(f"   memset({row_out}, 0, (size_t){bound} * sizeof *{row_out});")
            body.append(f"   for (int wire_row = 0; wire_row < {bound}; ++wire_row)")
            body.append("   {")
            slot = 0
            for index, (member, kind) in enumerate(members):
                at = f"wire_row * {width}u + {index}u"
                if kind in NUMERIC:
                    cell = f"wire_scratch[wire_row * {len(numeric)}u + {slot}u]"
                    body.append(f"      wire_values[{at}] = {cell};")
                    body.append(f"      wire_caps[{at}] = sizeof {cell};")
                    slot += 1
                elif member in row_allocated:
                    # Every row's allocation is made up front, because the
                    # frame is filled in one call: there is no point at which
                    # only the rows that arrived could be allocated. Rows past
                    # the reply are freed below rather than handed back.
                    ceiling = row_allocated[member]
                    cell = f"{row_out}[wire_row].{member}"
                    body.append(f"      {cell} = malloc({ceiling}u);")
                    body.append(f"      if (!{cell})")
                    body.append("      {")
                    body.append("         for (int wire_done = 0; wire_done < wire_row; ++wire_done)")
                    body.append("         {")
                    for other in row_allocated:
                        body.append(f"            free({row_out}[wire_done].{other});")
                        body.append(f"            {row_out}[wire_done].{other} = NULL;")
                    body.append("         }")
                    body.append("         free(wire_values);")
                    body.append("         free(wire_caps);")
                    body += ["         free(wire_scratch);"] if numeric else []
                    body += ["         free(wire_held);"] if allocates else []
                    body.append("         return -1;")
                    body.append("      }")
                    body.append(f"      {cell}[0] = '\\0';")
                    body.append(f"      wire_values[{at}] = {cell};")
                    body.append(f"      wire_caps[{at}] = {ceiling}u;")
                else:
                    cell = (f"{row_out}[wire_row]" if column
                            else f"{row_out}[wire_row].{member}")
                    body.append(f"      wire_values[{at}] = {cell};")
                    body.append(f"      wire_caps[{at}] = sizeof {cell};")
            body.append("   }")
            body.append("   uint32_t wire_filled = 0;")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, wire_values, wire_caps,")
            body.append(f"                           (uint32_t)({bound} * {width}), &wire_filled);")
            body.append("   free(wire_values);")
            body.append("   free(wire_caps);")
            fail_free = "".join(f"\n      free({o});" for o in (["wire_scratch"] if numeric else []))
            # A reply that is not a whole number of rows is not this operation's
            # reply, whatever its wire_status says.
            body.append(f"   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % {width}u != 0u)")
            body.append("   {" + fail_free)
            if row_allocated:
                # Partial failure releases what it took. The caller frees a row
                # it was given; it cannot free one it was never told about.
                body.append(f"      for (int wire_done = 0; wire_done < {bound}; ++wire_done)")
                body.append("      {")
                for other in row_allocated:
                    body.append(f"         free({row_out}[wire_done].{other});")
                    body.append(f"         {row_out}[wire_done].{other} = NULL;")
                body.append("      }")
            if allocates:
                body.append("      free(wire_held);")
            body.append("      return -1;")
            body.append("   }")
            body.append(f"   int wire_rows = (int)(wire_filled / {width}u);")
            if row_allocated:
                # The rows the reply did not fill were allocated all the same.
                # Handing them back would be memory the caller never asked for
                # and, past the returned count, never looks at to free.
                body.append(f"   for (int wire_row = wire_rows; wire_row < {bound}; ++wire_row)")
                body.append("   {")
                for member in row_allocated:
                    body.append(f"      free({row_out}[wire_row].{member});")
                    body.append(f"      {row_out}[wire_row].{member} = NULL;")
                body.append("   }")
                for member, ceiling in row_allocated.items():
                    body.append("   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)")
                    body.append("   {")
                    body.append(f"      char *wire_shrunk = realloc({row_out}[wire_row].{member}, "
                                f"strlen({row_out}[wire_row].{member}) + 1u);")
                    body.append("      if (wire_shrunk)")
                    body.append(f"         {row_out}[wire_row].{member} = wire_shrunk;")
                    body.append("   }")
            if numeric:
                body.append("   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)")
                body.append("   {")
                slot = 0
                for member, kind in members:
                    if kind in NUMERIC:
                        cell = f"wire_scratch[wire_row * {len(numeric)}u + {slot}u]"
                        target = (f"{row_out}[wire_row]" if column
                                  else f"{row_out}[wire_row].{member}")
                        body.append(f"      {target} = {numeric_parse(kind, cell)};")
                        slot += 1
                body.append("   }")
                body.append("   free(wire_scratch);")
            if allocates:
                body.append(f"   *{str(listed['out'])} = wire_held;")
            if counted:
                # The count is the caller's out-parameter here, so the return
                # goes back to saying only whether the call happened.
                body.append(f"   *{counted} = wire_rows;")
                body.append("   return 0;")
            else:
                body.append("   return wire_rows;")
        elif out_struct:
            members = [(str(f["name"]), str(f["type"])) for f in reply["fields"]]
            target = outputs[0]
            # A member the domain allocated is a member the client allocates:
            # the caller frees it with the same call it always did, and the
            # memory simply comes from this side of the bus now. Allocated at
            # the declared ceiling because the length is not known until the
            # reply lands, then shrunk to what came back.
            allocated = {str(f["name"]): int(f["alloc"])
                         for f in reply["fields"] if "alloc" in f}
            for index, (member, kind) in enumerate(members):
                if kind in NUMERIC:
                    body.append(f"   char slot{index}[{NUMERIC_TEXT}];")
            body.append(f"   memset({target}, 0, sizeof *{target});")
            for member, ceiling in allocated.items():
                body.append(f"   {target}->{member} = malloc({ceiling}u);")
                body.append(f"   if (!{target}->{member})")
                body.append("   {")
                for other in allocated:
                    body.append(f"      free({target}->{other});")
                body.append(f"      memset({target}, 0, sizeof *{target});")
                body.append("      return -1;")
                body.append("   }")
                body.append(f"   {target}->{member}[0] = '\\0';")
            slots = ", ".join(
                f"slot{index}" if kind in NUMERIC else f"{target}->{member}"
                for index, (member, kind) in enumerate(members))
            caps = ", ".join(
                f"sizeof slot{index}" if kind in NUMERIC
                else f"{allocated[member]}u" if member in allocated
                else f"sizeof {target}->{member}"
                for index, (member, kind) in enumerate(members))
            body.append(f"   char *const values[] = {{{slots}}};")
            body.append(f"   const size_t caps[] = {{{caps}}};")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, values, caps, "
                        f"{len(members)}, NULL);")
            if operation.get("c_returns") == "found":
                # This domain DOES distinguish, so the client hands back the
                # same three answers rather than folding nothing-there into
                # failure: a caller polling a queue would otherwise treat an
                # empty queue as a broken one and back off from it.
                body.append("   if (wire_status != (int)AIMEE_DB1_STATUS_OK)")
                body.append("   {")
                body += [f"      free({target}->{other});" for other in allocated]
                if allocated:
                    body.append(f"      memset({target}, 0, sizeof *{target});")
                body.append("      return wire_status == (int)AIMEE_DB1_STATUS_MISSING "
                            "? 0 : -1;")
                body.append("   }")
            else:
                # The domain answers 0 or -1 here: a miss and a failure are the
                # same answer to its callers, and the wire does not invent a
                # distinction the contract never had.
                body.append("   if (wire_status != (int)AIMEE_DB1_STATUS_OK)")
                body.append("   {")
                body += [f"      free({target}->{other});" for other in allocated]
                if allocated:
                    body.append(f"      memset({target}, 0, sizeof *{target});")
                body.append("      return -1;")
                body.append("   }")
            # Every numeric member, not a list of the kinds that existed when
            # this was written: a kind missing from that list is a member the
            # caller silently gets zero for.
            for index, (member, kind) in enumerate(members):
                if kind in NUMERIC:
                    body.append(f"   {target}->{member} = "
                                f"{numeric_parse(kind, f'slot{index}')};")
            for member in allocated:
                body.append(f"   char *shrunk_{member} = realloc({target}->{member}, "
                            f"strlen({target}->{member}) + 1u);")
                body.append(f"   if (shrunk_{member})")
                body.append(f"      {target}->{member} = shrunk_{member};")
            if operation.get("c_returns") == "member":
                body.append(f"   return {target}->{operation['c_member']};")
            else:
                body.append("   return 1;" if operation.get("c_returns") == "found"
                            else "   return 0;")
        elif reads and operation.get("c_returns") == "rc":
            # Two cells: what the caller asked for, and what the domain
            # answered about it. The second is not derivable from the first.
            body.append(f"   char slot_rc[{NUMERIC_TEXT}];")
            body.append(f"   char *const values[] = {{{outputs[0]}, slot_rc}};")
            body.append(f"   const size_t caps[] = {{{outputs[1]}, sizeof slot_rc}};")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, "
                        "values, caps, 2, NULL);")
            body.append("   if (wire_status != (int)AIMEE_DB1_STATUS_OK)")
            body.append("      return -1;")
            body.append("   return (int)strtol(slot_rc, NULL, 10);")
        elif reads:
            body.append(f"   char *const values[] = {{{outputs[0]}}};")
            body.append(f"   const size_t caps[] = {{{outputs[1]}}};")
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, values, caps, 1, NULL);")
            body.append(f"   return read_result(wire_status, {outputs[0]});")
        elif operation.get("c_returns") == "found":
            # A question whose whole answer is yes/no: nothing comes back but
            # the status, and the three answers stay three. Folding "no" into
            # "failed" would turn an empty result into an outage.
            body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, "
                        f"NULL, NULL, 0, NULL);")
            body.append("   if (wire_status == (int)AIMEE_DB1_STATUS_MISSING)")
            body.append("      return 0;")
            body.append("   return wire_status == (int)AIMEE_DB1_STATUS_OK ? 1 : -1;")
        elif returns_void:
            # The status is dropped deliberately, not by omission: the domain
            # never reported one, so there is nothing here to hand a caller and
            # no caller written to receive it.
            body.append(f"   (void)call_stage({op_symbol}, fields, {arity}, "
                        f"NULL, NULL, 0, NULL);")
        else:
            if renders_numbers:
                # The frame points into wire_rendered, so it is released after
                # the call rather than at the end of the loop that filled it.
                body.append(f"   int wire_status = call_stage({op_symbol}, fields, {arity}, "
                            f"NULL, NULL, 0, NULL);")
                body.append("   free(wire_rendered);")
                body.append("   return write_result(wire_status);")
            else:
                body.append(f"   return write_result(call_stage({op_symbol}, fields, {arity}, "
                            f"NULL, NULL, 0, NULL));")
        body.append("}")
        out.append("\n" + "\n".join(body) + "\n")
    out.append("\n/* clang-format on */\n")

    generated = "".join(out)
    reader = (READ_RESULT if "read_result(" in generated else "")
    writer = (WRITE_RESULT if "write_result(" in generated else "")
    return CLIENT_SCAFFOLD.format(
        stem=name, family=name.replace("_", " "), upper=upper,
        header=includes, client_doc=prose, write_result=writer,
        read_result=reader) + generated


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
                                domain_headers(root, operations), root)
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

{parse_int}{parse_int64}{parse_uint64}{parse_double}/* status(u32) | field_count(u32) | (len(u32) | bytes) * count. A write answers
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
      more fields than any operation takes writes past it. Zero is allowed:
      an operation that takes no arguments decodes no fields, and the arity
      check in its own case is what refuses a frame that carries some. */
   if (count > AIMEE_DB1_FIELDS_MAX)
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
   /* Set by an operation whose C return says 1 found / 0 nothing / negative
      failed. A row-returning domain usually answers 0 or -1 and has no such
      distinction; one that does must not have it flattened, or "the queue is
      empty" and "the queue is broken" reach the caller as the same answer. */
   int found = 0;
{row_locals}   /* A domain that returns a string hands over the allocation with it. The
      reply is written straight out of it rather than copied into value: the
      stack buffer is sized for identifiers and these carry documents. */
   char *text_owned = NULL;
   void *domain_rows = NULL;
   void *cells_owned = NULL;
   void *numeric_owned = NULL;
{scalar_pool}{member_pool}
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
   else if (found)
      status = (rc > 0) ? AIMEE_DB1_STATUS_OK
                        : (rc == 0 ? AIMEE_DB1_STATUS_MISSING : AIMEE_DB1_STATUS_FAILED);
   else if (rows)
      /* A row-returning domain usually answers 0 or -1: there is no
         found/not-found distinction to preserve, so neither is invented. */
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;
   else if (reads)
   {{
      if (rc < 0)
         status = AIMEE_DB1_STATUS_FAILED;
      else if (rc == 0 || !(text_owned ? text_owned : value)[0])
         status = AIMEE_DB1_STATUS_MISSING;
      else
         status = AIMEE_DB1_STATUS_OK;
   }}
   else
      status = (rc == 0) ? AIMEE_DB1_STATUS_OK : AIMEE_DB1_STATUS_FAILED;

   {{
      const char *held = text_owned ? text_owned : value;
      const char *one = (status == AIMEE_DB1_STATUS_OK) ? held : "";
      const char *const single[] = {{one}};
      const char *const *out_values = rows ? rows : (reads ? single : NULL);
      uint32_t out_count = rows ? row_count : (reads ? 1u : 0u);
      if (status != AIMEE_DB1_STATUS_OK && rows)
         out_count = 0u; /* nothing to report but the status */
      /* A reply that does not fit is a failure, not a success with nothing in
         it. write_reply refuses rather than truncating -- which is right -- but
         discarding that answer left the caller a well-formed frame carrying no
         rows, and a read cannot tell that from a row that is genuinely empty.
         Say it in the frame instead: a bare status needs eight bytes, so the
         second call fits wherever the first did not. */
      if (write_reply(response_body, response_capacity, response_len, status, out_values,
                      out_count) != status)
         write_reply(response_body, response_capacity, response_len, AIMEE_DB1_STATUS_FAILED,
                     NULL, 0u);
   }}
   free(cells_owned);
   free(numeric_owned);
{member_free}{scalar_free}{row_member_free}   free(domain_rows);
   free(text_owned);
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

PARSE_UINT64 = """/* The same, for a member the catalog declared unsigned. Signed parsing would
   accept "-1" and store it as the largest hash there is. */
static int parse_uint64(const char *text, uint64_t *out)
{
   if (!text || !text[0] || text[0] == '-')
      return 1;
   char *end = NULL;
   errno = 0;
   unsigned long long value = strtoull(text, &end, 10);
   if (errno != 0 || !end || *end != '\\0')
      return 1;
   *out = (uint64_t)value;
   return 0;
}

"""

PARSE_DOUBLE = """/* The same, for a value the catalog declared as a double. A cost parsed as an
   integer is a different number, and one that still looks like a price. */
static int parse_double(const char *text, double *out)
{
   if (!text || !text[0])
      return 1;
   char *end = NULL;
   errno = 0;
   double value = strtod(text, &end);
   if (errno != 0 || !end || *end != '\\0')
      return 1;
   *out = value;
   return 0;
}

"""

INT_INCLUDES = """#include <errno.h>
#include <limits.h>
#include <stdint.h>
"""


def stage_bytes(family: dict[str, object], operations: list[dict[str, object]],
                headers: list[str], root: Path) -> str:
    name = str(family["name"])
    cases = []
    for operation in operations:
        request = operation["request"]
        reply = operation["reply"]
        assert isinstance(request, dict) and isinstance(reply, dict)
        arity = len(request["fields"])
        types = [str(f["type"]) for f in request["fields"]]
        nullable = (lambda at: bool(request["fields"][at].get("null_when_empty"))
                    if at < len(request["fields"]) else False)
        names = [str(f["name"]) for f in request["fields"]]
        reads = bool(reply["fields"])
        in_struct = str(request["struct"]) if "struct" in request else ""
        out_struct = str(reply["struct"]) if "struct" in reply else ""
        parse, args, tail = [], [], []

        if in_struct:
            # Rebuild the row the caller flattened, then hand the domain the
            # struct it has always taken.
            in_struct_pointers = pointer_members(root, in_struct)
            # Anything before the struct's members is an ordinary argument that
            # is decoded like any other; the members start where the operation
            # says they do.
            lead = int(request["struct_from"]) if "struct_from" in request else 0
            for position, kind in enumerate(types[:lead]):
                if kind in NUMERIC:
                    conv = ("parse_int" if kind == "int"
                            else "parse_double" if kind in ("double", "float")
                            else "parse_uint64" if kind == "uint64" else "parse_int64")
                    ctype = ("int" if kind == "int"
                             else "double" if kind in ("double", "float")
                             else "uint64_t" if kind == "uint64" else "int64_t")
                    parse.append(f"      {ctype} parsed{position};\n"
                                 f"      if ({conv}(field[{position}], &parsed{position}) != 0)\n"
                                 f"      {{\n         free(scratch);\n"
                                 f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                                 f"      }}\n")
                    args.append(f"parsed{position}")
                else:
                    args.append(f"field[{position}]")
            parse.append(f"      {in_struct} row;\n      memset(&row, 0, sizeof row);\n")
            for position, (member, kind) in enumerate(zip(names, types)):
                if position < lead:
                    continue
                if kind in NUMERIC:
                    conv = ("parse_int" if kind == "int"
                            else "parse_double" if kind in ("double", "float")
                            else "parse_uint64" if kind == "uint64" else "parse_int64")
                    ctype = ("int" if kind == "int"
                             else "double" if kind in ("double", "float")
                             else "uint64_t" if kind == "uint64" else "int64_t")
                    parse.append(f"      {ctype} member_{position} = 0;\n"
                                 f"      if ({conv}(field[{position}], &member_{position}) != 0)\n"
                                 f"      {{\n         free(scratch);\n"
                                 f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                                 f"      }}\n"
                                 f"      row.{member} = member_{position};\n")
                else:
                    if member in in_struct_pointers:
                        # A const char * member is assigned, not copied into:
                        # sizeof would be the pointer's, and it is const. The
                        # field points into scratch, which outlives the call.
                        parse.append(f"      row.{member} = field[{position}];\n")
                    else:
                        parse.append(f"      snprintf(row.{member}, sizeof row.{member}, "
                                     f"\"%s\", field[{position}]);\n")
            args.append("&row")
        else:
            for position, kind in enumerate(types):
                if kind in NUMERIC:
                    conv = ("parse_int" if kind == "int"
                            else "parse_double" if kind in ("double", "float")
                            else "parse_uint64" if kind == "uint64" else "parse_int64")
                    ctype = ("int" if kind == "int"
                             else "double" if kind in ("double", "float")
                             else "uint64_t" if kind == "uint64" else "int64_t")
                    args.append(f"parsed{position}")
                    parse.append(f"      {ctype} parsed{position};\n"
                                 f"      if ({conv}(field[{position}], &parsed{position}) != 0)\n"
                                 f"      {{\n"
                                 f"         free(scratch);\n"
                                 f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                                 f"      }}\n")
                elif nullable(position):
                    # "" is how an absent string arrives; this parameter is one
                    # whose NULL means something else entirely.
                    args.append(f"field[{position}][0] ? field[{position}] : NULL")
                else:
                    args.append(f"field[{position}]")

        listed = reply.get("list")
        repeated = request.get("repeated")
        scalars = reply.get("scalars")
        returns_id = operation.get("c_returns") == "int64"
        returns_text = operation.get("c_returns") == "text"
        if returns_id:
            returned = declared_return(root, str(operation["c_name"])).strip()
            if returned in ("double", "float"):
                # A double has no negative failure value to test: the domain
                # reports trouble by answering zero, which is what it always
                # did, so the call happening is the success.
                tail.append("      rc = 0;\n")
                tail.append('      snprintf(row_text[0], sizeof row_text[0], "%.17g", '
                            "(double)produced);\n")  # a float widens losslessly
            elif str(operation.get("negatives", "")) == "data":
                # Declared: every value this returns is an answer, so the only
                # failure left is the store not answering at all.
                tail.append("      rc = 0;\n")
                tail.append('      snprintf(row_text[0], sizeof row_text[0], "%lld", '
                            "(long long)produced);\n")
            else:
                tail.append("      rc = (produced >= 0) ? 0 : -1;\n")
                tail.append('      snprintf(row_text[0], sizeof row_text[0], "%lld", '
                            "(long long)produced);\n")
            tail.append("      row_slots[0] = row_text[0];\n")
            tail.append("      rows = row_slots;\n      row_count = 1u;\n")
        beside = 1 if (returns_text and scalars) else 0
        if scalars:
            # The domain writes into locals; the reply is those locals rendered.
            # A text value gets a stage-side buffer: the caller's capacity is
            # its own business, and the reply is bounded by what the stage
            # declares it can produce.
            # They live at function scope for the same reason a row's does: the
            # values array escapes the case and write_reply reads it afterwards.
            # The document, when there is one, is the first field and not a
            # value the domain writes into: everything here indexes past it.
            scalar_fields = reply["fields"][1 if (returns_text and scalars) else 0:]
            members = [(str(f["name"]), str(f["type"])) for f in scalar_fields]
            stage_alloc = {i for i, f in enumerate(scalar_fields) if "alloc" in f}
            widths = [str(scalar_fields[i]["width"])
                      for i, (_, kind) in enumerate(members)
                      if kind == "text" and i not in stage_alloc]
            if widths:
                parse.append(f"      scalar_owned = calloc(1u, {' + '.join(widths)});\n"
                             "      if (!scalar_owned)\n      {\n"
                             "         free(scratch);\n"
                             "         return AIMEE_MODULE_STATUS_INTERNAL;\n      }\n")
                offset = ""
                for index, (_, kind) in enumerate(members):
                    if kind != "text" or index in stage_alloc:
                        continue
                    width = str(scalar_fields[index]["width"])
                    parse.append(f"      char *scalar{index} = scalar_owned{offset};\n")
                    offset += f" + {width}"
            for index, (member, kind) in enumerate(members):
                if index in stage_alloc:
                    # The domain allocates this one. The stage owns it from the
                    # moment it lands: written out with the rest of the reply,
                    # then given back after write_reply has read it.
                    parse.append(f"      char *scalar{index} = NULL;\n")
                    args.append(f"&scalar{index}")
                elif kind == "text":
                    width = str(scalar_fields[index]["width"])
                    args += [f"scalar{index}", f"(size_t){width}"]
                else:
                    ctype = {"int": "int", "int64": "int64_t", "double": "double", "float": "float"}[kind]
                    spelled_out = declared_parameters(
                        root, str(operation.get("c_name", ""))).get(
                            str(operation["c_params"][len(request["fields"]) + index])
                            if "c_params" in operation else "")
                    if spelled_out:
                        ctype = spelled_out.rsplit("*", 1)[0].strip()
                    parse.append(f"      {ctype} scalar{index} = 0;\n")
                    args.append(f"&scalar{index}")
            for index, (member, kind) in enumerate(members):
                if index in stage_alloc:
                    tail.append(f"      member_owned[{sorted(stage_alloc).index(index)}] = "
                                f"scalar{index};\n")
                    tail.append(f"      row_slots[{index + beside}] = scalar{index} "
                                f"? scalar{index} : \"\";\n")
                elif kind == "text":
                    tail.append(f"      row_slots[{index + beside}] = scalar{index};\n")
                else:
                    spec, cast = numeric_format(kind)
                    tail.append(f"      snprintf(row_text[{index}], sizeof row_text[{index}], "
                                f"\"{spec}\", {cast}scalar{index});\n")
                    tail.append(f"      row_slots[{index + beside}] = row_text[{index}];\n")
            if beside:
                tail.append("      row_slots[0] = produced ? produced : \"\";\n")
                # rc is 1 when the document was there, so the reply is read the
                # way a found/nothing/failed one is. Without this the row-shaped
                # default reads "found" as a failure -- every hit would come
                # back as a miss.
                tail.append("      found = 1;\n")
            tail.append(f"      rows = row_slots;\n"
                        f"      row_count = {len(members) + beside}u;\n")
        if returns_text:
            # The domain hands back memory. The stage owns it from here: copy it
            # into the reply and free it, and refuse rather than truncate when it
            # will not fit -- a half a context is not a shorter context, it is a
            # different one, and the caller cannot tell.
            tail.append("      text_owned = produced;\n")
        if listed:
            members = [(str(f["name"]), str(f["type"])) for f in reply["fields"]]
            width = len(members)
            numeric = [i for i, (_, k) in enumerate(members) if k in NUMERIC]
            row_out = str(listed["out"])
            stage_counted = str(listed["count"]) if "count" in listed else ""
            bound = str(listed["bound"]) if "bound" in listed else ""
            column = listed.get("column")
            excluded_params = {row_out} | ({stage_counted} if stage_counted else set())
            if repeated:
                excluded_params |= {str(repeated["values"]), str(repeated["count"])}
            inputs = [p for p in operation["c_params"] if p not in excluded_params]
            if bound:
                at = inputs.index(bound)
                held = args[at]
                # The bound is the allocation, so it is checked against the
                # ceiling the catalog declares before anything is allocated
                # from it. A stage that took the caller's word would size an
                # array from the wire.
                parse.append(f"      if ({held} <= 0 || {held} > {listed['max_rows']})\n"
                             f"      {{\n         free(scratch);\n"
                             f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n      }}\n")
            else:
                # No caller bound crossed, so the ceiling is the catalog's own
                # and the stage says so out loud rather than trusting a count
                # it did not receive.
                held = str(listed["max_rows"])
            row_decl = (
                (f"char (*found)[{column['width']}]" if str(column["kind"]) == "text"
                 else f"{'int64_t' if column['kind'] == 'int64' else 'int'} *found")
                if column else f"{out_struct} *found")
            if listed.get("allocate"):
                parse.append(f"      {row_decl} = NULL;\n")
            else:
                parse.append(f"      {row_decl} = calloc((size_t){held}, sizeof *found);\n"
                             f"      if (!found)\n"
                             f"      {{\n         free(scratch);\n"
                             f"         return AIMEE_MODULE_STATUS_INTERNAL;\n      }}\n"
                             f"      domain_rows = found;\n")
            ordered = dict(zip(inputs, args))
            ordered[row_out] = "&found" if listed.get("allocate") else "found"
            if stage_counted:
                parse.append("      int produced_rows = 0;\n")
                ordered[stage_counted] = "&produced_rows"
            if repeated:
                # field[] is already const char *[], contiguous and decoded, so
                # the domain takes a pointer into it rather than a copy. It
                # lives until scratch is freed, which is after the call.
                ordered[str(repeated["values"])] = f"&field[{len(request['fields'])}]"
                ordered[str(repeated["count"])] = f"(int)(count - {len(request['fields'])}u)"
            args = [ordered[p] for p in operation["c_params"]]
            listed_alloc = {str(f["name"]) for f in reply["fields"] if "alloc" in f}
            assigns = "".join(
                f"            cells[row * {width}u + {i}u] = "
                + (f"numbers[row * {len(numeric)}u + {numeric.index(i)}u];\n"
                   if kind in NUMERIC
                   # A member the domain allocated may be NULL, which has always
                   # meant empty. write_reply cannot take a NULL cell.
                   else (f"found[row].{member} ? found[row].{member} : \"\";\n"
                         if member in listed_alloc
                         else (f"found[row];\n" if column else f"found[row].{member};\n")))
                for i, (member, kind) in enumerate(members))
            converts = "".join(
                f"            snprintf(numbers[row * {len(numeric)}u + {numeric.index(i)}u], {NUMERIC_TEXT},\n"
                f"                     \"{numeric_format(kind)[0]}\", "
                f"{numeric_format(kind)[1]}"
                f"{'found[row]' if column else f'found[row].{member}'});\n"
                for i, (member, kind) in enumerate(members) if kind in NUMERIC)
            numbers = (f"         char (*numbers)[{NUMERIC_TEXT}] = malloc((size_t)produced * "
                       f"{len(numeric)}u * sizeof *numbers);\n" if numeric else "")
            guard = "!cells" + (" || !numbers" if numeric else "")
            release = "            free(cells);\n" + ("            free(numbers);\n" if numeric else "")
            if listed.get("allocate"):
                tail.append("      domain_rows = found;\n")
            if stage_counted:
                # The domain reported how many through a parameter and answered
                # only whether it worked. The frame carries rows, so the count
                # becomes the answer here and a failure stays a failure.
                tail.append("      rc = (rc == 0) ? produced_rows : -1;\n")
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
            # Function scope, not case scope. `rows` escapes the switch and is
            # read by write_reply below, so anything it points at has to still
            # exist there: the row struct itself, the text a numeric member was
            # rendered into, and the pointer array. Declared inside the case
            # these are dead the moment it breaks, and the compiler is free to
            # put the locals that follow in the same stack slots -- which is
            # exactly what it did.
            slot = f"row_{out_struct}"
            parse.append(f"      memset(&{slot}, 0, sizeof {slot});\n")
            if "out" in reply and "c_params" in operation:
                # The out parameter is not always last. Ordering the call by
                # c_params rather than by append order is the same fix the
                # client needed, and the stage got it wrong the same way.
                row_out = str(reply["out"])
                by_name = dict(zip([p for p in operation["c_params"] if p != row_out], args))
                by_name[row_out] = f"&{slot}"
                args = [by_name[p] for p in operation["c_params"]]
            else:
                args.append(f"&{slot}")
            allocated_reply = {str(f["name"]): position
                               for position, f in enumerate(
                                   [g for g in reply["fields"] if "alloc" in g])}
            numeric = 0
            for index, (member, kind) in enumerate(members):
                if kind in NUMERIC:
                    spec, cast = numeric_format(kind)
                    tail.append(f"      snprintf(row_text[{numeric}], sizeof row_text[{numeric}], "
                                f"\"{spec}\", {cast}{slot}.{member});\n")
                    numeric += 1
            numeric = 0
            for index, (member, kind) in enumerate(members):
                if kind in NUMERIC:
                    tail.append(f"      row_slots[{index}] = row_text[{numeric}];\n")
                    numeric += 1
                elif member in allocated_reply:
                    # The domain handed over an allocation with the row. It is
                    # written out and then returned: NULL is the empty value it
                    # has always meant, and write_reply cannot take a NULL cell.
                    tail.append(f"      member_owned[{allocated_reply[member]}] = "
                                f"{slot}.{member};\n")
                    tail.append(f"      row_slots[{index}] = {slot}.{member} "
                                f"? {slot}.{member} : \"\";\n")
                else:
                    tail.append(f"      row_slots[{index}] = {slot}.{member};\n")
            tail.append(f"      rows = row_slots;\n      row_count = {len(members)}u;\n")
        elif reads and not (returns_text or returns_id or scalars):
            # The buffer-and-cap pair belongs to a plain read alone. Every other
            # shape supplies its own out-parameters, and appending these on top
            # of them is simply two extra arguments to the domain call.
            args += ["value", "sizeof value"]
            if operation.get("c_returns") == "rc":
                # The status says the call happened; the second cell says what
                # it answered. A read whose text is always present cannot carry
                # its answer in whether the text is present.
                tail.append('      snprintf(row_text[0], sizeof row_text[0], "%d", rc);\n')
                tail.append("      row_slots[0] = value;\n")
                tail.append("      row_slots[1] = row_text[0];\n")
                tail.append("      rows = row_slots;\n      row_count = 2u;\n")
                tail.append("      rc = 0;\n")
        rep_span = len(repeated.get("fields", [])) if repeated else 0
        if repeated and "kind" in repeated and not listed:
            # A bare array of numbers: decoded into storage the stage owns and
            # handed over as the array the domain has always taken.
            base = len(request["fields"])
            ctype = "int" if str(repeated["kind"]) == "int" else "int64_t"
            conv = "parse_int" if str(repeated["kind"]) == "int" else "parse_int64"
            parse.append(f"      int repeated_rows = (int)(count - {base}u);\n"
                         f"      {ctype} *repeated_held = "
                         f"calloc((size_t)repeated_rows + 1u, sizeof *repeated_held);\n"
                         f"      if (!repeated_held)\n      {{\n         free(scratch);\n"
                         f"         return AIMEE_MODULE_STATUS_INTERNAL;\n      }}\n"
                         f"      domain_rows = repeated_held;\n"
                         f"      for (int at = 0; at < repeated_rows; ++at)\n      {{\n"
                         f"         if ({conv}(field[{base} + at], &repeated_held[at]) != 0)\n"
                         f"         {{\n            free(scratch);\n"
                         f"            return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                         f"         }}\n      }}\n")
            ordered = dict(zip([p for p in operation["c_params"]
                                if p not in {str(repeated["values"]), str(repeated["count"])}],
                               args))
            ordered[str(repeated["values"])] = "repeated_held"
            ordered[str(repeated["count"])] = "repeated_rows"
            args = [ordered[p] for p in operation["c_params"]]
        if repeated and rep_span and not listed:
            # Rebuild the rows the client flattened, hand the domain the array
            # it has always taken, and give it back afterwards.
            base = len(request["fields"])
            row_type = str(repeated["struct"])
            parse.append(f"      int repeated_rows = (int)((count - {base}u) / {rep_span}u);\n"
                         f"      {row_type} *repeated_held = "
                         f"calloc((size_t)repeated_rows + 1u, sizeof *repeated_held);\n"
                         f"      if (!repeated_held)\n      {{\n         free(scratch);\n"
                         f"         return AIMEE_MODULE_STATUS_INTERNAL;\n      }}\n"
                         f"      domain_rows = repeated_held;\n"
                         f"      for (int at = 0; at < repeated_rows; ++at)\n      {{\n")
            for index, member in enumerate(repeated["fields"]):
                cell = f"field[{base} + at * {rep_span} + {index}]"
                mname, mkind = str(member["name"]), str(member["type"])
                if mkind in NUMERIC:
                    conv = ("parse_int" if mkind == "int"
                            else "parse_double" if mkind in ("double", "float")
                            else "parse_int64")
                    ctype = ("int" if mkind == "int"
                             else "double" if mkind in ("double", "float") else "int64_t")
                    parse.append(f"         {ctype} member_{index} = 0;\n")
                    parse.append(f"         if ({conv}({cell}, &member_{index}) != 0)\n"
                                 f"         {{\n            free(scratch);\n"
                                 f"            return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                                 f"         }}\n")
                    parse.append(f"         repeated_held[at].{mname} = member_{index};\n")
                else:
                    parse.append(f"         snprintf(repeated_held[at].{mname}, "
                                 f"sizeof repeated_held[at].{mname}, \"%s\", {cell});\n")
            parse.append("      }\n")
            ordered = dict(zip([p for p in operation["c_params"]
                                if p not in {str(repeated["values"]), str(repeated["count"])}],
                               args))
            ordered[str(repeated["values"])] = "repeated_held"
            ordered[str(repeated["count"])] = "repeated_rows"
            args = [ordered[p] for p in operation["c_params"]]
        # Braced only when a parsed integer needs scoping: an empty block around
        # every other case would be noise, and would move files that have not
        # changed.
        # The ceiling counts CELLS, and a repeated row is several cells each.
        ceiling = int(repeated["max_values"]) * (rep_span or 1) if repeated else 0
        check = ((f"      if (count > {ceiling}u)\n" if not arity
                  else f"      if (count < {arity}u || count > {arity}u + {ceiling}u)\n")
                 if repeated else f"      if (count != {arity}u)\n")
        if rep_span:
            # A frame that is not a whole number of rows is not this
            # operation's frame, whatever else it satisfies.
            check += (f"      {{\n         free(scratch);\n"
                      f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n      }}\n"
                      f"      if ((count - {arity}u) % {rep_span}u != 0u)\n")
        body = (check
                + f"      {{\n"
                f"         free(scratch);\n"
                f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n"
                f"      }}\n"
                + "".join(parse)
                + (f"      char *produced = {operation['c_name']}({', '.join(args)});\n"
                   "      rc = produced ? 1 : 0;\n" if returns_text
                   else f"      {declared_return(root, str(operation['c_name'])).strip()} "
                        f"produced = {operation['c_name']}({', '.join(args)});\n"
                   if returns_id
                   # A void domain has no rc to take. It reports failure the
                   # only way it ever did -- by not having happened -- so the
                   # stage answers OK for a request it accepted and delivered.
                   # rc starts at -1, so saying so is not optional: without it
                   # every void call replies FAILED, which a void client
                   # discards and a void call WITH an out-parameter does not.
                   else f"      {operation['c_name']}({', '.join(args)});\n"
                        f"      rc = 0;\n"
                   if operation.get("c_returns") == "void"
                   else f"      rc = {operation['c_name']}({', '.join(args)});\n")
                + "".join(tail)
                # Not for a list: it declares member fields like a row does, but
                # an empty list must answer with no values, where a read answers
                # with one. Setting this would turn "nothing found" into a
                # single empty string.
                + ("      reads = 1;\n"
                   if reads and not (listed or scalars or returns_id) else "")
                + "      break;\n")
        head = f"   case AIMEE_DB1_OP_{str(operation['name']).upper()}:\n"
        needs = "".join(
            f"      if (!field[{i}][0])\n      {{\n         free(scratch);\n"
            f"         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n      }}\n"
            for i, f in enumerate(request["fields"]) if f["required"])
        marker = "         return AIMEE_MODULE_STATUS_INVALID_REQUEST;\n      }\n"
        cut = body.index(marker) + len(marker)
        body = body[:cut] + needs + body[cut:]
        # Braced when anything declares a local: a declaration straight after a
        # case label is not portable C, and the returned string is one.
        if operation.get("c_returns") in ("found", "member"):
            # Both say the same thing about the domain's return: it is the
            # answer rather than a status, so a positive value is success and
            # the row-shaped default -- which reads anything but 0 as a
            # failure -- would throw away every id it was handed.
            # Before the break, not after it: appending to the body would put
            # the assignment past the jump, where it is dead and every "no"
            # from the domain reaches the caller as "yes".
            closing = "      break;\n"
            assert body.endswith(closing), name
            body = body[:-len(closing)] + "      found = 1;\n" + closing
        cases.append(head + (f"   {{\n{body}   }}\n"
                             if parse or returns_text or returns_id else body))
    used = {str(f["type"]) for o in operations for f in o["request"]["fields"]}
    typed = bool(used & {"int", "int64", "uint64", "double", "float"})
    # Storage for a single row's reply, at function scope because write_reply
    # reads it after the switch. One variable per distinct row type the family
    # answers with; a union would save stack but would have to invent member
    # names for types that already have them.
    row_types, widest, most_numeric = [], 0, 0
    for operation in operations:
        reply = operation["reply"]
        if operation.get("c_returns") == "int64":
            widest = max(widest, 1)
            most_numeric = max(most_numeric, 1)
            continue
        if operation.get("c_returns") == "rc":
            # The value and the return: two slots, one of them rendered from a
            # number the reply never declared as a field.
            widest = max(widest, 2)
            most_numeric = max(most_numeric, 1)
            continue
        if "scalars" in reply:
            # No struct, but the same escaping arrays: every value is numeric,
            # and row_slots still outlives the case that fills it.
            widest = max(widest, len(reply["fields"]))
            most_numeric = max(most_numeric, len(reply["fields"]))
            continue
        if "struct" not in reply or "list" in reply:
            continue
        struct = str(reply["struct"])
        if struct not in row_types:
            row_types.append(struct)
        widest = max(widest, len(reply["fields"]))
        most_numeric = max(most_numeric, sum(1 for f in reply["fields"]
                                             if str(f["type"]) in NUMERIC))
    # Only families that actually produce a text scalar carry the pool: an
    # unused local in every other stage is churn in files nothing changed.
    pooled = any("scalars" in o["reply"] and any(str(f["type"]) == "text"
                                                 for f in o["reply"]["fields"])
                 for o in operations)
    scalar_pool = ("   /* Text scalars are written by the domain and read after the switch\n"
                   "      closes, so their storage cannot live in a case block. One\n"
                   "      allocation holds all of an operation's values end to end, and one\n"
                   "      free returns it. */\n"
                   "   char *scalar_owned = NULL;\n") if pooled else ""
    scalar_free = "   free(scalar_owned);\n" if pooled else ""
    # A row whose members the domain allocated: they are written out and then
    # returned, after write_reply has copied what it needs.
    owned = max((sum(1 for f in o["reply"]["fields"] if "alloc" in f)
                 for o in operations if "list" not in o["reply"]), default=0)
    member_pool = ("   /* Members the domain allocated with the row. They are released\n"
                   "      after the reply is written, not before: write_reply reads them. */\n"
                   f"   char *member_owned[{owned}] = {{0}};\n") if owned else ""
    member_free = (f"   for (size_t slot = 0; slot < {owned}u; ++slot)\n"
                   "      free(member_owned[slot]);\n") if owned else ""
    # Rows whose members the domain allocated. The switch is on op rather than
    # on a flag set in the case: the stage already knows what it served, and a
    # second variable saying the same thing is a second thing to get wrong.
    freeing = []
    for operation in operations:
        reply = operation["reply"]
        if "list" not in reply or not any("alloc" in f for f in reply["fields"]):
            continue
        width = len(reply["fields"])
        freeing.append(
            f"   case AIMEE_DB1_OP_{str(operation['name']).upper()}:\n"
            "      if (domain_rows)\n      {\n"
            f"         {reply['struct']} *held = domain_rows;\n"
            f"         for (uint32_t at = 0; at < row_count / {width}u; ++at)\n"
            "         {\n"
            + "".join(f"            free(held[at].{f['name']});\n"
                      for f in reply["fields"] if "alloc" in f)
            + "         }\n      }\n      break;\n")
    row_member_free = ("   switch (op)\n   {\n" + "".join(freeing)
                       + "   default:\n      break;\n   }\n") if freeing else ""
    row_locals = ""
    if row_types or widest:
        row_locals = "".join(f"   {struct} row_{struct};\n" for struct in row_types)
        row_locals += f"   const char *row_slots[{widest}];\n"
        if most_numeric:
            row_locals += f"   char row_text[{most_numeric}][{NUMERIC_TEXT}];\n"
    return STAGE_SCAFFOLD.format(stem=name, family=name.replace("_", " "),
                                 row_locals=row_locals,
                                 scalar_pool=scalar_pool, scalar_free=scalar_free,
                                 member_pool=member_pool, member_free=member_free,
                                 row_member_free=row_member_free,
                                 headers="\n".join(f'#include "{h}"' for h in headers),
                                 cases="".join(cases),
                                 parse_int=PARSE_INT if "int" in used else "",
                                 parse_int64=PARSE_INT64 if "int64" in used else "",
                                 parse_double=PARSE_DOUBLE if used & {"double", "float"} else "",
                                 parse_uint64=PARSE_UINT64 if "uint64" in used else "",
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
              stage_bytes(family, operations, domain_headers(root, operations), root)
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
    catalog = validate_catalog(load_json(root / CATALOG), root)
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
