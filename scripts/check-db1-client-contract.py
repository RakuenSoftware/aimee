#!/usr/bin/env python3
"""The C clients must ask db1 for the shape the catalog declares.

src/db1_client is 16,500 lines of hand-written encode/call/decode across 461
operations, and the generator that used to produce it went with the C store. So
nothing checks it against anything: a client that asks for one cell where the
module answers two, or sends four fields where the module expects five, compiles
and links and fails at runtime as a refusal with no explanation.

That is not hypothetical. Eleven operations shipped exactly that way -- the
client asked for the value AND the entry point's rc, the module answered only
the value, and call_stage treats an OK reply narrower than the slots it asked
for as a contract mismatch and returns -1. Every one of those calls could not
succeed. They were found by reading the C by hand against the catalog, which is
not a repeatable way to find the next one.

The catalog is the one description of the wire that neither side wrote. This
compares both numbers in every call_stage against it:

  * the request arity -- how many fields the client sends
  * the reply width  -- how many cells it has room for, per row

A list asks for max_rows * width and checks `filled % width`, so its width is
the same catalog number as a scalar's; only the spelling differs.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT_DIR = ROOT / "src" / "db1_client"
CATALOG = ROOT / "server-go" / "modules" / "aimee" / "operations.json"
MODULE_API = CLIENT_DIR / "db1_module_api.h"

# call_stage(op, fields, count, values, caps, slots, filled)
CALL = re.compile(
    r"call_stage\(\s*AIMEE_DB1_OP_(?P<op>[A-Z0-9_]+)\s*,"
    r"\s*(?P<fields>[^,]+?)\s*,"
    r"\s*(?P<count>[^,]+?)\s*,"
    r"\s*(?P<values>[^,]+?)\s*,"
    r"\s*(?P<caps>[^,]+?)\s*,"
    r"\s*(?P<slots>.+?)\s*,\s*(?:NULL|&\w+)\s*\)",
    re.S,
)

# A list's slot count: max_rows * row_width, however the cast is spelled.
LIST_SLOTS = re.compile(r"\*\s*(\d+)\s*\)?\s*$")
KIND_DEFINE = re.compile(r"^#define AIMEE_DB1_EVENT_([A-Z0-9_]+)\s+(\d+)u", re.M)


def flat_arity(fields: list[dict]) -> int:
    """Slots these fields occupy on the wire; the wire is flat."""
    total = 0
    for f in fields:
        repeat = f.get("repeat", 1)
        width = flat_arity(f["fields"]) if f.get("type") == "struct" else 1
        total += repeat * width
    return total


def load_catalog() -> tuple[dict[str, dict], dict[str, int]]:
    doc = json.loads(CATALOG.read_text())
    ops = {}
    for op in doc["operations"]:
        request = op.get("request", {})
        ops[op["name"]] = {
            "request": flat_arity(request.get("fields", [])),
            "reply": flat_arity(op.get("reply", {}).get("fields", [])),
            "family": op["family"],
            # A repeated block makes the arity variable: a fixed prefix plus n
            # copies of a struct. There is no single number to hold the client
            # to, and the operation validates its own shape instead.
            "variadic": "repeated" in request,
        }
    families = {f["name"]: f["event_kind"] for f in doc["families"]}
    return ops, families


def literal_count(expr: str) -> int | None:
    """A request arity, when the client states one outright.

    Only a bare number. `1 + n * 6` is a variadic call and has no single arity
    to compare -- reading a 6 out of it is how this check first reported a
    false positive.
    """
    expr = expr.strip()
    return int(expr) if expr.isdigit() else None


def literal_slots(expr: str) -> int | None:
    """A reply width, per row.

    A scalar states it outright. A list asks for max_rows * width and then
    checks `filled % width`, so the width is the number to compare -- and it is
    the same catalog number either way.
    """
    expr = expr.strip()
    if expr.isdigit():
        return int(expr)
    m = LIST_SLOTS.search(expr)
    return int(m.group(1)) if m else None


def main() -> int:
    if not CATALOG.is_file():
        print(f"check-db1-client-contract: no catalog at {CATALOG}", file=sys.stderr)
        return 1
    ops, families = load_catalog()

    mismatches: list[str] = []
    unknown: list[str] = []
    checked = skipped = 0

    # GUARD THE GUARD. This is the only thing comparing 461 C call sites against
    # the catalog, and without this it would report "ok (0 call sites)" and exit
    # zero the moment the client directory moved or was renamed -- which is
    # precisely what happened to the store module's own tree today. A check that
    # passes because it found nothing to check is worse than no check: it makes
    # the tree look verified.
    sources = sorted(CLIENT_DIR.glob("*.c"))
    if not sources:
        print(f"check-db1-client-contract: no client sources at {CLIENT_DIR}; "
              f"this check would pass having compared nothing", file=sys.stderr)
        return 2

    for path in sources:
        text = path.read_text(errors="ignore")
        for m in CALL.finditer(text):
            name = m.group("op").lower()
            spec = ops.get(name)
            if spec is None:
                unknown.append(f"{path.name}: {m.group('op')} is not in the catalog")
                continue

            want_req, want_reply = spec["request"], spec["reply"]
            got_req = literal_count(m.group("count"))
            got_reply = literal_slots(m.group("slots"))
            if got_reply is None:
                # A computed reply width cannot be compared without running it.
                skipped += 1
                continue
            checked += 1

            if got_req is not None and not spec["variadic"] and got_req != want_req:
                mismatches.append(
                    f"{path.name}: {name} sends {got_req} fields, "
                    f"the catalog's request expands to {want_req}")
            if got_reply != want_reply:
                mismatches.append(
                    f"{path.name}: {name} has room for {got_reply} reply cells, "
                    f"the catalog declares {want_reply}")

    # The kinds the clients compile against must be the ones the catalog assigns.
    kind_problems = []
    if MODULE_API.is_file():
        for name, kind in KIND_DEFINE.findall(MODULE_API.read_text()):
            family = name.lower()
            want = families.get(family)
            if want is None:
                continue
            if int(kind) != want:
                kind_problems.append(
                    f"db1_module_api.h: {family} is kind {kind}, "
                    f"the catalog assigns {want}")

    problems = mismatches + kind_problems
    if problems:
        print("db1 client contract: the C clients disagree with the catalog.")
        print("A call that asks for the wrong shape does not fail to build; it")
        print("fails at runtime as a refusal with nothing to debug from.")
        print()
        for p in problems:
            print(f"  {p}")
        if unknown:
            print()
            for u in unknown[:10]:
                print(f"  {u}")
        return 1

    note = f", {skipped} with computed arities" if skipped else ""
    print(f"check-db1-client-contract: ok ({checked} call sites{note}, "
          f"{len(families)} kinds)")
    if unknown:
        print(f"  {len(unknown)} operations named by the client are not in the catalog:")
        for u in unknown[:5]:
            print(f"    {u}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
