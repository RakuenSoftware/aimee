"""Add reviewed DB2 operations to the event contract from one batch file.

Every operation added by hand touched seven places: the catalog, the review
ledger, the generator's table, the adapter's vtable, its binding, its handler,
and the two tests. Six of those are mechanical -- they follow from the schema
and the backend signature -- and the seventh, the review reason, is the only
part that needs a person. Doing all seven by hand meant the mechanical six
consumed the attention the seventh deserved.

A batch file is JSON: a list of operations, each carrying its family, its
identifier, the backend symbol it replaces, its request and reply schema, the
policy the catalog records, the review reason, and the one expression that calls
the backend. Everything else follows.

    python3 scripts/db2_add_operations.py batch.json

The batch file's own shape is checked before anything is written, so a batch
either applies completely or not at all. Run the generator and the tests
afterwards; this writes sources, it does not validate them.

One field needs explaining. `call` is the expression the adapter's handler uses
to reach the backend, written against the decoded request fields and whatever
the reply needs. It cannot be derived, because the backend's argument order and
its out-parameters are its own business, and pretending otherwise would produce
a call that compiles and means something else.
"""
import bisect
import json
import re
import sys
from pathlib import Path

CATALOG = Path("src/modules/db2/eventcontract/operations.json")
REVIEW = Path("src/modules/db2/eventcontract/declaration-review.json")
GENERATOR = Path("scripts/gen_db2_contract.py")
LEDGER = Path("tests/baselines/db2/declarations-v1.json")
ADAPTER_HEADER = Path("src/modules/db2/module_adapter.h")
ADAPTER_SOURCE = Path("src/modules/db2/module_adapter.c")
CONTRACT_TEST = Path("src/tests/test_db2_module_contract.c")
BUS_TEST = Path("src/tests/test_bus_db2_module.c")

REQUIRED = {"name", "family", "id", "symbol", "request", "reply", "policy", "reason", "call",
            "vtable", "stub"}
OPTIONAL = {"scope", "transaction", "idempotency", "results", "db3_reason", "handler_extra",
            "also"}


def fail(message: str) -> None:
    print(f"db2_add_operations: {message}", file=sys.stderr)
    raise SystemExit(1)


def check(batch: list[dict[str, object]]) -> None:
    """Reject a batch before anything is written."""
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    taken = {(str(item["family"]), int(item["id"])) for item in catalog["operations"]}
    names = {str(item["name"]) for item in catalog["operations"]}
    ledger = {row["symbol"] for row in json.loads(LEDGER.read_text(encoding="utf-8"))
              ["declarations"]}
    families = {str(item["name"]) for item in catalog["families"]}
    for operation in batch:
        missing = REQUIRED - set(operation)
        if missing:
            fail(f"{operation.get('name', '?')} is missing {sorted(missing)}")
        unknown = set(operation) - REQUIRED - OPTIONAL
        if unknown:
            fail(f"{operation['name']} carries unknown keys {sorted(unknown)}")
        if operation["family"] not in families:
            fail(f"{operation['name']} names family {operation['family']!r}, which does not exist")
        key = (str(operation["family"]), int(operation["id"]))
        if key in taken:
            fail(f"{operation['name']} claims {key}, which is taken")
        taken.add(key)
        if operation["name"] in names:
            fail(f"{operation['name']} is already a catalog operation")
        names.add(str(operation["name"]))
        if operation["symbol"] not in ledger:
            fail(f"{operation['name']} names {operation['symbol']!r}, not a DB2 declaration")
        if len(str(operation["reason"]).encode("utf-8")) > 512:
            fail(f"{operation['name']} has a reason longer than the ledger allows")


    # The Go contract test reads every fixture into one struct, so a field name
    # is shared across operations and can carry only one type. A name reused at
    # a different type produces a struct that cannot hold both, and it fails as
    # a type error inside a generated test rather than as anything about the
    # batch, so it is caught here instead.
    typed: dict[str, str] = {}
    for item in catalog["operations"]:
        if item["wire_format"] != "db2-envelope-generic-v1":
            continue
        for field in item["request"]["fields"]:
            typed[str(field["name"])] = str(field["type"])
    for operation in batch:
        for field in operation["request"]:
            name, kind = str(field["name"]), str(field["type"])
            if typed.get(name, kind) != kind:
                fail(f"{operation['name']} names a field {name!r} of type {kind}, but another "
                     f"operation already carries {name!r} as {typed[name]}; give it a name of "
                     "its own")
            typed[name] = kind

    # A field's bound constant is named for the operation and the field, with
    # nothing to say which half of the operation it belongs to, so a request
    # field and a reply row field of the same name collide in the header.
    for operation in batch:
        request_names = {str(field["name"]) for field in operation["request"]}
        reply = operation["reply"]
        reply_fields = reply["row"]["fields"] if "row" in reply else reply["fields"]
        shared = request_names & {str(field["name"]) for field in reply_fields}
        if shared:
            fail(f"{operation['name']} names {sorted(shared)} in both its request and its reply; "
                 "their bound constants would collide, so one of them needs its own name")


def catalog_entry(operation: dict[str, object]) -> str:
    body = {
        "family": operation["family"],
        "id": operation["id"],
        "name": operation["name"],
        "wire_format": "db2-envelope-generic-v1",
        "scope": operation.get("scope", "none"),
        "transaction": operation.get("transaction", "none"),
        "idempotency": operation.get("idempotency", "safe"),
        "results": operation.get("results", ["ok"]),
        "db3_placement": "retained-db2",
        "db3_reason": operation.get("db3_reason", operation["reason"][:200]),
        "c_symbols": [operation["symbol"]] + [str(item["symbol"])
                                              for item in operation.get("also", [])],
        "request": {"policy": operation["policy"], "fields": operation["request"]},
        "reply": operation["reply"],
    }
    rendered = json.dumps(body, indent=2)
    return "".join(f"    {line}\n" for line in rendered.split("\n"))


def apply_catalog(batch: list[dict[str, object]]) -> None:
    """Insert each operation where the catalog's own order puts it.

    The catalog is sorted by family and then by operation id, and a gate checks
    it, so appending is only correct when the batch happens to belong at the
    end. Each entry goes before the first operation that sorts after it.
    """
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    order = {str(item["name"]): index for index, item in enumerate(catalog["families"])}
    existing = sorted((order[str(item["family"])], int(item["id"]), str(item["name"]))
                      for item in catalog["operations"])

    text = CATALOG.read_text(encoding="utf-8")
    for operation in sorted(batch,
                            key=lambda item: (order[str(item["family"])], int(item["id"])),
                            reverse=True):
        position = (order[str(operation["family"])], int(operation["id"]))
        following = next((name for family, identifier, name in existing
                          if (family, identifier) > position), None)
        entry = catalog_entry(operation).rstrip("\n")
        if following is None:
            tail = "    }\n  ]\n}\n"
            if text.count(tail) != 1:
                fail("the catalog does not end where expected")
            text = text.replace(tail, "    },\n" + entry + "\n  ]\n}\n")
        else:
            # The newline in front makes this a whole line: a field named
            # after an operation is indented further, and without the leading
            # newline the deeper line still contains this one as a substring.
            anchor = f'\n      "name": "{following}",\n'
            if text.count(anchor) != 1:
                fail(f"cannot place {operation['name']} before {following}")
            start = text.rindex("    {\n", 0, text.index(anchor))
            text = text[:start] + entry + ",\n" + text[start:]
        existing.append((*position, str(operation["name"])))
        existing.sort()
    CATALOG.write_text(text, encoding="utf-8")
    json.loads(CATALOG.read_text(encoding="utf-8"))


def apply_review(batch: list[dict[str, object]]) -> None:
    ledger = {row["symbol"]: row for row in
              json.loads(LEDGER.read_text(encoding="utf-8"))["declarations"]}
    text = REVIEW.read_text(encoding="utf-8")
    blocks = list(re.finditer(r'    \{\n      "symbol": "(\w+)",.*?\n    \}', text, re.S))
    symbols = [match.group(1) for match in blocks]
    if symbols != sorted(symbols):
        fail("the review ledger is not sorted by symbol")
    additions = []
    for operation in batch:
        # An operation's own symbol first, then any name that is a forward to
        # it: each needs its own review row, because the ledger is a list of
        # declarations and a forward is one.
        reviewed = [(str(operation["symbol"]), operation["reason"])]
        reviewed += [(str(item["symbol"]), item["reason"])
                     for item in operation.get("also", [])]
        for symbol, reason in reviewed:
            block = ('    {\n'
                     f'      "symbol": "{symbol}",\n'
                     f'      "signature_sha256": "{ledger[symbol]["signature_sha256"]}",\n'
                     '      "disposition": "wire-operation",\n'
                     f'      "family": "{operation["family"]}",\n'
                     '      "db3_placement": "retained-db2",\n'
                     f'      "reason": {json.dumps(reason)}\n'
                     '    }')
            additions.append((bisect.bisect_left(symbols, symbol), symbol, block))
    for position, _symbol, block in sorted(additions, reverse=True):
        text = text[:blocks[position].start()] + block + ",\n" + text[blocks[position].start():]
    json.loads(text)
    REVIEW.write_text(text, encoding="utf-8")


def _forwards(operation: dict[str, object]) -> str:
    """The generator table's list of names that forward to this backend."""
    names = [str(item["symbol"]) for item in operation.get("also", [])]
    if not names:
        return ""
    joined = ", ".join(repr(name) for name in names)
    return f'\n        "forwards": ({joined},),'


def apply_generator(batch: list[dict[str, object]]) -> None:
    text = GENERATOR.read_text(encoding="utf-8")
    rows = "".join(f'''    "{operation['name']}": {{
        "key": ("{operation['family']}", {operation['id']}),
        "format": "db2-envelope-generic-v1",
        "symbol": "{operation['symbol']}",
        "policy": {{{", ".join(f'"{key}": 200' for key in operation["policy"])}}},{_forwards(operation)}
    }},
''' for operation in batch)
    anchor = "}\n\n\ndef _check_derived("
    if text.count(anchor) != 1:
        fail("the generator's table does not end where expected")
    GENERATOR.write_text(text.replace(anchor, rows + anchor), encoding="utf-8")
    compile(GENERATOR.read_text(encoding="utf-8"), str(GENERATOR), "exec")


def apply_adapter(batch: list[dict[str, object]]) -> None:
    header = ADAPTER_HEADER.read_text(encoding="utf-8")
    anchor = "} aimee_db2_module_backend_t;"
    if header.count(anchor) != 1:
        fail("the adapter's backend table does not end where expected")
    members = "".join(f"   {operation['vtable']}\n" for operation in batch)
    ADAPTER_HEADER.write_text(header.replace(anchor, members + anchor), encoding="utf-8")

    source = ADAPTER_SOURCE.read_text(encoding="utf-8")
    for operation in batch:
        name = str(operation["name"])
        member = str(operation["vtable"]).split("(*")[1].split(")")[0]
        bind_anchor = "   };\n   return &backend;"
        if source.count(bind_anchor) != 1:
            fail("the production backend does not end where expected")
        source = source.replace(
            bind_anchor, f"       .{member} = {operation['symbol']},\n" + bind_anchor)
    ADAPTER_SOURCE.write_text(source, encoding="utf-8")


def apply_stubs(batch: list[dict[str, object]]) -> None:
    anchor = "int db2_anti_pattern_exists_exact(const char *pattern)\n"
    stubs = ""
    for operation in batch:
        stubs += f"{operation['stub']}\n\n"
        # A forward is a separate C symbol, so the tests need it linked too.
        for item in operation.get("also", []):
            stubs += f"{item['stub']}\n\n"
    for path in (CONTRACT_TEST, BUS_TEST):
        text = path.read_text(encoding="utf-8")
        if text.count(anchor) != 1:
            fail(f"{path.name} has no stub anchor")
        path.write_text(text.replace(anchor, stubs + anchor), encoding="utf-8")



def _c_locals(operation: dict[str, object], fields: list[dict[str, object]],
              prefix: str) -> tuple[str, str]:
    """Declarations and decode arguments for one field list."""
    upper = str(operation["name"]).upper()
    declarations, arguments = [], []
    for field in fields:
        name, kind = str(field["name"]), field["type"]
        bound = f"AIMEE_DB2_{upper}_{name.upper()}_MAX"
        if kind == "utf8":
            declarations.append(f"         char {name}[{bound} + 1] = \"\";")
            arguments.append(f"{name}, sizeof({name})")
        elif kind == "u32":
            declarations.append(f"         uint32_t {name} = 0u;")
            arguments.append(f"&{name}")
        elif kind == "u64":
            declarations.append(f"         uint64_t {name} = 0u;")
            arguments.append(f"&{name}")
        else:
            declarations.append(f"         double {name} = 0.0;")
            arguments.append(f"&{name}")
    return "\n".join(declarations), ", ".join(arguments)


# What a row's widest encoding may reach before the handler allocates it
# instead of declaring it. Matches the client's own bound.
STACK_ROW_MAX = 16384


def _row_bytes(operation: dict[str, object]) -> int:
    """The widest row list one described operation fills."""
    row = operation["reply"]["row"]
    total = 0
    for field in row["fields"]:
        kind = field["type"]
        if kind == "utf8":
            total += 4 + int(field["maximum_bytes"])
        elif kind == "u32":
            total += 4
        else:
            total += 8
    return total * int(operation["reply"]["maximum_rows"])


def handler_block(operation: dict[str, object]) -> str:
    """The adapter branch one described operation needs."""
    name = str(operation["name"])
    upper = name.upper()
    member = str(operation["vtable"]).split("(*")[1].split(")")[0]
    request_declarations, request_arguments = _c_locals(operation, operation["request"], "")
    # A request with no fields decodes from the envelope alone, so there is
    # nothing to declare and nothing to pass after the buffer.
    request_arguments = f", {request_arguments}" if request_arguments else ""
    request_declarations = f"{request_declarations}\n" if request_declarations else ""

    reply = operation["reply"]
    release = ""
    if "row" in reply:
        if _row_bytes(operation) > STACK_ROW_MAX:
            reply_declarations = (
                f"         aimee_db2_{name}_row_t *rows =\n"
                f"             malloc(sizeof(*rows) * AIMEE_DB2_{upper}_MAX_ROWS);\n"
                "         uint32_t count = 0u;\n"
                "         if (!rows)\n"
                "            return AIMEE_MODULE_STATUS_INTERNAL;")
            release = "            free(rows);\n"
        else:
            reply_declarations = (
                f"         aimee_db2_{name}_row_t rows[AIMEE_DB2_{upper}_MAX_ROWS];\n"
                "         uint32_t count = 0u;")
        encode = (f"aimee_db2_{name}_reply_encode(rows, count, response_body, "
                  "response_capacity, response_len)")
    else:
        declarations = []
        names = []
        for field in reply["fields"]:
            field_name, kind = str(field["name"]), field["type"]
            bound = f"AIMEE_DB2_{upper}_{field_name.upper()}_MAX"
            if kind == "utf8":
                declarations.append(f"         char {field_name}[{bound} + 1] = \"\";")
            elif kind == "u32":
                declarations.append(f"         uint32_t {field_name} = 0u;")
            elif kind == "u64":
                declarations.append(f"         uint64_t {field_name} = 0u;")
            else:
                declarations.append(f"         double {field_name} = 0.0;")
            names.append(field_name)
        reply_declarations = "\n".join(declarations)
        encode = (f"aimee_db2_{name}_reply_encode({', '.join(names)}, response_body, "
                  "response_capacity, response_len)")

    extra = operation.get("handler_extra", "")
    extra = f"\n{extra}" if extra else ""
    return f"""      {{
{request_declarations}         if (aimee_db2_{name}_request_decode(request_body, request_len{request_arguments}) == 0)
         {{
            if (response_capacity < AIMEE_DB2_{upper}_RESPONSE_MAX_LEN)
               return AIMEE_MODULE_STATUS_INVALID_REQUEST;
            if (!backend || !backend->{member})
               return AIMEE_MODULE_STATUS_CAPABILITY_ABSENT;
{reply_declarations}
{operation['call']}
            if (aimee_module_invocation_cancelled(invocation))
            {{
{release}               return AIMEE_MODULE_STATUS_CANCELLED;
            }}{extra}
            if ({encode} != 0)
            {{
{release}               return AIMEE_MODULE_STATUS_INTERNAL;
            }}
{release}            return AIMEE_MODULE_STATUS_OK;
         }}
      }}
"""


# Where each family's described operations are inserted. Every family branch
# ends with a decoder that is tried last, so a new block goes before it.
FAMILY_ANCHORS = {
    "memory": ("      uint32_t promotions = 0u, demotions = 0u, expirations = 0u;\n"
               "      if (aimee_db2_health_record_request_decode(request_body, request_len, "
               "&promotions, &demotions,\n"
               "                                                 &expirations) == 0)\n"),
    "index": ("      if (aimee_db2_entity_edge_normalize_weights_request_decode(request_body, "
              "request_len) == 0)\n"),
    "learning": ("      if (aimee_db2_proposals_archive_expired_request_decode(request_body, "
                 "request_len) == 0)\n"),
    "organization": "      char project[AIMEE_DB2_CLEAR_PROJECT_PROJECT_MAX + 1] = {0};\n",
    "maintenance": ("      if (aimee_db2_prospective_sweep_expired_request_decode(request_body, "
                    "request_len) == 0)\n"),
    "custody": ("   if (invocation->stage_id == AIMEE_DB2_STAGE_VECTOR_REBUILD_LOCK_TRY_ACQUIRE)\n"
                "   {\n"),
}


def apply_handlers(batch: list[dict[str, object]]) -> None:
    """Insert one handler block per operation, in its own family's branch."""
    source = ADAPTER_SOURCE.read_text(encoding="utf-8")
    for family in sorted({str(operation["family"]) for operation in batch}):
        anchor = FAMILY_ANCHORS.get(family)
        if anchor is None:
            fail(f"no handler anchor for family {family!r}")
        if source.count(anchor) != 1:
            fail(f"the {family} branch does not start where expected")
        blocks = "".join(handler_block(operation) for operation in batch
                         if operation["family"] == family)
        # Custody's branch opens with its condition, so its blocks go after it;
        # every other anchor is the decoder the new blocks precede.
        source = (source.replace(anchor, anchor + blocks) if family == "custody"
                  else source.replace(anchor, blocks + anchor))
    ADAPTER_SOURCE.write_text(source, encoding="utf-8")



def apply_includes(batch: list[dict[str, object]]) -> None:
    """Include the backend header for every symbol the batch reaches.

    Which header declares a symbol is discovered rather than declared in the
    batch: it is a fact about the tree, and asking a batch to repeat it would
    be one more thing to get wrong.
    """
    source = ADAPTER_SOURCE.read_text(encoding="utf-8")
    headers = sorted(Path("src/modules/db2/c").glob("*.h"))
    wanted: set[str] = set()
    for operation in batch:
        symbol = str(operation["symbol"])
        for header in headers:
            body = header.read_text(encoding="utf-8", errors="replace")
            if re.search(rf"\b{re.escape(symbol)}\s*\(", body):
                wanted.add(f'#include "c/{header.name}"')
                break
        else:
            fail(f"no header in src/modules/db2/c declares {symbol}")

    # The vtable in the adapter's header names the row types those same
    # headers declare, so it needs them too -- by a path relative to itself.
    header_text = ADAPTER_HEADER.read_text(encoding="utf-8")
    header_missing = sorted(
        include.replace('#include "c/', '#include "') for include in wanted
        if include.replace('#include "c/', '#include "') not in header_text)
    if header_missing:
        header_anchor = '#include "entity_edges.h"\n'
        if header_text.count(header_anchor) != 1:
            fail("the adapter header's include block does not start where expected")
        ADAPTER_HEADER.write_text(
            header_text.replace(header_anchor, header_anchor + "".join(
                f"{include}\n" for include in header_missing)), encoding="utf-8")

    missing = sorted(include for include in wanted if include not in source)
    if not missing:
        return
    anchor = '#include "c/db2.h"\n'
    if source.count(anchor) != 1:
        fail("the adapter's include block does not start where expected")
    ADAPTER_SOURCE.write_text(
        source.replace(anchor, anchor + "".join(f"{include}\n" for include in missing)),
        encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    batch = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    if not isinstance(batch, list) or not batch:
        fail("a batch is a non-empty list of operations")
    check(batch)
    apply_catalog(batch)
    apply_review(batch)
    apply_generator(batch)
    apply_includes(batch)
    apply_adapter(batch)
    apply_handlers(batch)
    apply_stubs(batch)
    print(f"db2_add_operations: added {len(batch)} operation(s); "
          "run the generator, then the tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
