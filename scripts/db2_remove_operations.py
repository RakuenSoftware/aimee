"""Withdraw operations from the event contract, reversing db2_add_operations.

Adding an operation touches seven places. Withdrawing one has to touch the same
seven, and doing it by hand leaves a symbol reviewed in one file and pending in
another -- which the gate catches, eventually, after the parts that do not
catch it have been rebuilt several times.

Withdrawal is not a rare event. An operation is published on a reading of what
its backend needs, the replay then shows the reading was incomplete, and the
honest response is to take it back rather than leave it published and wrong.
The first three withdrawals were of operations whose answer depends on a
session principal the envelope cannot carry: they replayed clean only because
the replay connects as a superuser and row-level security does not apply to
one.

    python3 scripts/db2_remove_operations.py <operation-name> [<operation-name> ...]

The named operations are removed from the catalog, the review ledger, the
generator's table, the adapter's vtable and handler, and the stubs in both unit
tests. Replay cases in src/tests/test_bus_db2_process.c are NOT removed -- they
name the generated client, so the compiler finds them, and a person should see
what the withdrawn case asserted before deleting it.

Run the generator afterwards. This writes sources; it does not validate them.
"""
import json
import re
import sys
from pathlib import Path

CATALOG = Path("src/modules/db2/eventcontract/operations.json")
REVIEW = Path("src/modules/db2/eventcontract/declaration-review.json")
GENERATOR = Path("scripts/gen_db2_contract.py")
ADAPTER_HEADER = Path("src/modules/db2/module_adapter.h")
ADAPTER_SOURCE = Path("src/modules/db2/module_adapter.c")
CONTRACT_TEST = Path("src/tests/test_db2_module_contract.c")
BUS_TEST = Path("src/tests/test_bus_db2_module.c")


def fail(message: str) -> None:
    print(f"db2_remove_operations: {message}", file=sys.stderr)
    raise SystemExit(1)


def catalogued(names: list[str]) -> list[dict]:
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    found = {str(item["name"]): item for item in catalog["operations"]}
    missing = [name for name in names if name not in found]
    if missing:
        fail(f"not in the catalog: {missing}")
    wrong = [name for name in names
             if found[name]["wire_format"] != "db2-envelope-generic-v1"]
    if wrong:
        fail(f"not generic-envelope operations, so this cannot reverse them: {wrong}")
    return [found[name] for name in names]


def strip_catalog(names: list[str]) -> None:
    """Remove each entry, and the comma that joined it to its neighbour."""
    text = CATALOG.read_text(encoding="utf-8")
    for name in names:
        anchor = f'\n      "name": "{name}",\n'
        if text.count(anchor) != 1:
            fail(f"cannot locate {name} in the catalog")
        start = text.rindex("    {\n", 0, text.index(anchor))
        end = text.index("\n    }", text.index(anchor)) + len("\n    }")
        if text[end:end + 2] == ",\n":
            end += 2                      # it had a successor
        else:
            start = text.rindex("},\n", 0, start) + len("}")  # it was the last
        text = text[:start] + text[end:]
    CATALOG.write_text(text, encoding="utf-8")
    json.loads(CATALOG.read_text(encoding="utf-8"))


def strip_review(symbols: set[str]) -> None:
    review = json.loads(REVIEW.read_text(encoding="utf-8"))
    before = len(review["reviews"])
    review["reviews"] = [row for row in review["reviews"] if row["symbol"] not in symbols]
    if before - len(review["reviews"]) != len(symbols):
        fail("the review ledger does not hold exactly one row per withdrawn symbol")
    REVIEW.write_text(json.dumps(review, indent=2, ensure_ascii=False) + "\n",
                      encoding="utf-8")


def strip_generator(names: list[str]) -> None:
    text = GENERATOR.read_text(encoding="utf-8")
    for name in names:
        pattern = re.compile(rf'    "{re.escape(name)}": \{{\n.*?\n    \}},\n', re.S)
        if len(pattern.findall(text)) != 1:
            fail(f"cannot locate {name} in the generator's table")
        text = pattern.sub("", text, count=1)
    GENERATOR.write_text(text, encoding="utf-8")
    compile(GENERATOR.read_text(encoding="utf-8"), str(GENERATOR), "exec")


def strip_adapter(operations: list[dict], members: dict[str, str]) -> None:
    header = ADAPTER_HEADER.read_text(encoding="utf-8")
    for operation in operations:
        member = members[str(operation["name"])]
        pattern = re.compile(rf"^   [^\n]*\(\*{re.escape(member)}\)\([^;]*?\);\n", re.M | re.S)
        if len(pattern.findall(header)) != 1:
            fail(f"cannot locate the vtable member {member}")
        header = pattern.sub("", header, count=1)
    ADAPTER_HEADER.write_text(header, encoding="utf-8")

    source = ADAPTER_SOURCE.read_text(encoding="utf-8")
    for operation in operations:
        member = members[str(operation["name"])]
        binding = f"       .{member} = {operation['c_symbols'][0]},\n"
        if source.count(binding) != 1:
            fail(f"cannot locate the backend binding for {member}")
        source = source.replace(binding, "", 1)
    ADAPTER_SOURCE.write_text(source, encoding="utf-8")


def strip_handlers(names: list[str]) -> None:
    """Remove each handler block: the braced scope holding its decoder call.

    The generated shape is a bare block at one indentation, opening with the
    reply's locals and calling `<name>_request_decode`. Finding the block by
    matching braces from the `{` before that call is what keeps this honest --
    the blocks nest, so a search for the next closing brace at the same column
    would stop inside one.
    """
    source = ADAPTER_SOURCE.read_text(encoding="utf-8")
    for name in names:
        call = f"aimee_db2_{name}_request_decode("
        if source.count(call) != 1:
            fail(f"cannot locate the handler for {name} ({source.count(call)} decoders)")
        start = source.rindex("\n      {\n", 0, source.index(call)) + 1
        depth = 0
        index = start
        while True:
            character = source[index]
            if character == "{":
                depth += 1
            elif character == "}":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        end = source.index("\n", index) + 1
        source = source[:start] + source[end:]
    ADAPTER_SOURCE.write_text(source, encoding="utf-8")


def strip_stubs(operations: list[dict]) -> None:
    for path in (CONTRACT_TEST, BUS_TEST):
        text = path.read_text(encoding="utf-8")
        for operation in operations:
            for symbol in operation["c_symbols"]:
                pattern = re.compile(
                    rf"^[A-Za-z_][^\n]*\b{re.escape(symbol)}\s*\([^;{{]*?\)\n\{{\n.*?\n\}}\n\n",
                    re.M | re.S)
                matches = pattern.findall(text)
                if len(matches) != 1:
                    fail(f"cannot locate the {symbol} stub in {path.name} "
                         f"({len(matches)} matches)")
                text = pattern.sub("", text, count=1)
        path.write_text(text, encoding="utf-8")


def main() -> int:
    names = sys.argv[1:]
    if not names:
        print(__doc__, file=sys.stderr)
        return 2
    operations = catalogued(names)
    # Which vtable member belongs to an operation is not written down anywhere:
    # it is the one the production backend binds to the operation's symbol.
    source = ADAPTER_SOURCE.read_text(encoding="utf-8")
    members: dict[str, str] = {}
    for operation in operations:
        symbol = operation["c_symbols"][0]
        found = re.findall(rf"^       \.(\w+) = {re.escape(symbol)},$", source, re.M)
        if len(found) != 1:
            fail(f"cannot tell which vtable member binds {symbol}")
        members[str(operation["name"])] = found[0]

    strip_handlers(names)
    strip_adapter(operations, members)
    strip_stubs(operations)
    strip_generator(names)
    strip_review({symbol for operation in operations for symbol in operation["c_symbols"]})
    strip_catalog(names)
    print(f"db2_remove_operations: withdrew {len(names)} operation(s); "
          "run the generator, then remove their replay cases by hand")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
