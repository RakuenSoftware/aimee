#!/usr/bin/env python3
"""Measure what the remaining DB1 operations need from the module wire.

Reports, for every DB1 function that a linked binary still calls: the capability
tier its signature needs, whether its callers pass NULL or an empty string, and
whether it carries a field the current size cap would refuse.

This is a measurement, not a gate. It reads the headers, the Makefile and the
call sites, so its numbers move when any of those move -- which is the point.
docs/proposals/pending/db1-wire-capability-survey.md records what it said on the
day it was written; re-run this before trusting those numbers again.

Two counting mistakes are deliberately designed out, because both were made:

  * Signatures are read from the HEADERS with line joins collapsed. Reading
    definitions line by line silently drops every wrapped signature.
  * CMD_SRCS is excluded. Those files are compiled by cmd-srcs-compile-check so
    they cannot rot, but they are linked into nothing, and counting them as
    production inflates the surface by about a fifth.
"""

from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
CATALOG = Path("src/modules/db1/eventcontract/operations.json")
SOURCE_DIR = Path("src/modules/db1")
MAKEFILE = Path("src/Makefile")

DECL = re.compile(
    r"\b(int|void|int64_t|double|size_t|char\s*\*|const\s+char\s*\*)\s+"
    r"([a-z][a-z0-9_]{3,})\s*\(([^;{]*)\)\s*;", re.S)
CALL = re.compile(r"\b([a-z][a-z0-9_]{2,})\s*\(")

TEXT = re.compile(r"const char \s*\*\s*\w+$")
SCALAR = re.compile(r"(int|int64_t|long|unsigned int|unsigned|time_t|double) \w+$")
OUT_TEXT = re.compile(r"char \s*\*\s*\w+$")
CAP = re.compile(r"size_t \w+$")
OUT_NUM = re.compile(r"(int|int64_t|double|size_t) \s*\*\s*\w+$")
STRUCT_IN = re.compile(r"const [a-z_0-9]+_t \s*\*\s*\w+$")
STRUCT_OUT = re.compile(r"[a-z_0-9]+_t \s*\*\s*\w+$")
ARRAY_LEN = re.compile(r"(int|size_t) (max|n|count|cap|limit)\w*$")
# Names that carry a prompt, a result or a document rather than an identifier.
LARGE = re.compile(r"(json|metadata|content|prompt|body|payload|text|summary|blob"
                   r"|result|output|detail|notes?|message|reason|sql|patch|diff)", re.I)

TIER_OF = {"text": 0, "out_text": 0, "int": 1, "out_num": 2}
TIER_NAME = {
    0: "T0  fits the wire today",
    1: "T1  + integer arguments",
    2: "T2  + a richer reply (counted, multi-value, or a status beyond ok/miss/fail)",
    3: "T3  + structured payloads (struct, array, malloc'd out)",
}

# A contract that answers with more than success/failure or found/not-found.
# The wire maps a write to 0/-1 and a read to 1/0/-1, so an operation that
# returns a count, or a distinguished refusal like the single-writer -2, loses
# the distinction it exists to make.
RICH_RETURN = re.compile(r"(\B-2\b|\B-3\b|number of|count of|returns the number|how many)",
                         re.I)


def compile_only_sources(root: Path) -> set[str]:
    """CMD_SRCS entries that are named nowhere else, so nothing links them."""
    makefile = (root / MAKEFILE).read_text(encoding="utf-8")
    block = re.search(r"^CMD_SRCS\s*[:+]?=(.*?)(?=^\w)", makefile, re.M | re.S)
    if not block:
        return set()
    declared = set(re.findall(r"([A-Za-z0-9_/.\-]+)\.c", block.group(1)))
    rest = re.sub(r"^CMD_SRCS\s*[:+]?=.*?(?=^\w)", "", makefile, flags=re.M | re.S)
    # A stem counts as linked if it appears anywhere else as a .c OR a .o: some
    # command objects are linked only through an explicit object list.
    elsewhere = {Path(x).name for x in re.findall(r"([A-Za-z0-9_/.\-]+)\.[co]\b", rest)}
    return {s for s in declared if Path(s).name not in elsewhere}


def reserved_sources(root: Path) -> dict[str, str]:
    catalog = json.loads((root / CATALOG).read_text(encoding="utf-8"))
    return {source: family["name"]
            for family in catalog["families"] if not family["active"]
            for source in family["sources"]}


def declarations(root: Path, families: dict[str, str]) -> dict[str, dict]:
    found: dict[str, dict] = {}
    for header in sorted((root / SOURCE_DIR).glob("*.h")):
        if header.stem not in families:
            continue
        raw = header.read_text(errors="ignore")
        # The comment immediately before a declaration is its contract, and the
        # contract is where a count or a distinguished refusal is stated.
        documented: dict[str, str] = {}
        for pair in re.finditer(r"/\*(.*?)\*/\s*((?:[^;/]|/(?!\*))*?;)", raw, re.S):
            named = re.search(r"\b([a-z][a-z0-9_]{3,})\s*\(", pair.group(2))
            if named:
                documented[named.group(1)] = pair.group(1)
        text = re.sub(r"/\*.*?\*/", "", raw, flags=re.S)
        for match in DECL.finditer(text):
            found[match.group(2)] = {
                "family": families[header.stem],
                "source": header.stem,
                "returns": " ".join(match.group(1).split()),
                "params": " ".join(match.group(3).split()),
                "contract": " ".join(documented.get(match.group(2), "").split()),
            }
    return found


def classify(params: str) -> list[str]:
    raw = params.strip()
    parts = [p.strip() for p in raw.split(",")] if raw not in ("void", "") else []
    tags, index = [], 0
    while index < len(parts):
        current = parts[index]
        following = parts[index + 1] if index + 1 < len(parts) else ""
        if following and OUT_TEXT.search(current) and CAP.search(following):
            tags.append("out_text")
            index += 2
            continue
        if following and STRUCT_OUT.search(current) and ARRAY_LEN.search(following):
            tags.append("out_array")
            index += 2
            continue
        if TEXT.search(current):
            tags.append("text")
        elif SCALAR.search(current):
            tags.append("int")
        elif STRUCT_IN.search(current):
            tags.append("struct_in")
        elif OUT_NUM.search(current):
            tags.append("out_num")
        elif STRUCT_OUT.search(current) or OUT_TEXT.search(current):
            tags.append("struct_out")
        else:
            tags.append("other")
        index += 1
    return tags


def split_call(text: str, start: int) -> list[str] | None:
    """Split one call's arguments, respecting nesting, strings and escapes."""
    index = text.index("(", start)
    depth, current, found, in_string, escaped = 0, "", [], False, False
    while index < len(text):
        char = text[index]
        if in_string:
            current += char
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
            current += char
        elif char == "(":
            depth += 1
            if depth > 1:
                current += char
        elif char == ")":
            depth -= 1
            if depth == 0:
                found.append(current.strip())
                return found
            current += char
        elif char == "," and depth == 1:
            found.append(current.strip())
            current = ""
        else:
            current += char
        index += 1
    return None


def survey(root: Path) -> dict:
    families = reserved_sources(root)
    declared = declarations(root, families)
    skip = compile_only_sources(root)

    callers: dict[str, set[str]] = collections.defaultdict(set)
    for path in sorted((root / "src").rglob("*.c")):
        relative = path.relative_to(root / "src").as_posix()
        if relative.startswith("modules/db1/") or "/tests/" in f"/{relative}":
            continue
        if relative[:-2] in skip:
            continue
        text = path.read_text(errors="ignore")
        for name in set(CALL.findall(text)):
            if name in declared:
                callers[name].add(relative)

    operations = {}
    for name, sites in callers.items():
        entry = dict(declared[name], callers=sorted(sites))
        entry["tags"] = classify(entry["params"])
        tier = 0
        for tag in entry["tags"]:
            tier = max(tier, TIER_OF.get(tag, 3))
        if entry["returns"] in ("char *", "const char *"):
            tier = 3
        # An operation taking nothing cannot be framed: the request carries at
        # least one counted field.
        if not entry["tags"]:
            tier = max(tier, 2)
        # One reply value, so a second out buffer has nowhere to go.
        if entry["tags"].count("out_text") > 1:
            tier = max(tier, 2)
        if RICH_RETURN.search(entry.get("contract", "")):
            tier = max(tier, 2)
            entry["rich_return"] = True
        entry["tier"] = tier
        names = [p.split()[-1].lstrip("*") for p in entry["params"].split(",")]
        entry["large_fields"] = sorted({
            field for tag, field in zip(entry["tags"], names)
            if tag == "text" and LARGE.search(field)})
        operations[name] = entry

    # Nullability, for the operations that could migrate today.
    for name, entry in operations.items():
        if entry["tier"] > 1:
            entry["nullable"] = []
            continue
        positions = [i for i, tag in enumerate(entry["tags"]) if tag == "text"]
        seen = set()
        for relative in entry["callers"]:
            text = (root / "src" / relative).read_text(errors="ignore")
            for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
                arguments = split_call(text, match.start())
                if not arguments or len(arguments) != len(entry["tags"]):
                    continue
                for position in positions:
                    value = arguments[position].strip()
                    if value in ("NULL", '""'):
                        seen.add((relative, position, value))
        entry["nullable"] = sorted(seen)
    return operations


def report(operations: dict) -> None:
    total = len(operations)
    tiers = collections.Counter(o["tier"] for o in operations.values())
    print(f"DB1 operations with a linked caller: {total}")
    print(f"caller files involved              : "
          f"{len({c for o in operations.values() for c in o['callers']})}\n")
    running = 0
    for tier in sorted(tiers):
        running += tiers[tier]
        print(f"  {TIER_NAME[tier]:52} {tiers[tier]:>4}   cumulative "
              f"{running:>3} ({100 * running // total}%)")

    ready = {n: o for n, o in operations.items() if o["tier"] <= 1}
    nullable = {n: o for n, o in ready.items() if o["nullable"]}
    large = {n: o for n, o in ready.items() if o["large_fields"]}
    print(f"\n  reachable now (T0+T1)                : {len(ready)}")
    # Both of these were blockers once and are not now. They stay in the report
    # because they say what the reachable set is carrying -- a family full of
    # documents is a different migration from a family full of identifiers --
    # but neither refuses a call any more.
    print(f"    passing NULL or \"\" somewhere      : {len(nullable)}"
          f"   (carried: fields declare required)")
    print(f"    carrying a prompt/result/document  : {len(large)}"
          f"   (carried: requests are not capped)")

    print("\n  by family (ready / total):")
    families = collections.defaultdict(collections.Counter)
    for entry in operations.values():
        families[entry["family"]][entry["tier"]] += 1
    for family in sorted(families, key=lambda f: -sum(families[f].values())):
        counts = families[family]
        print(f"    {family:14} {counts[0] + counts[1]:>3} / {sum(counts.values()):<3}"
              f"   behind T2 {counts[2]:>2}   behind T3 {counts[3]:>2}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--json", action="store_true", help="emit the raw survey")
    args = parser.parse_args(argv)
    operations = survey(args.root.resolve())
    if args.json:
        json.dump(operations, sys.stdout, indent=1, sort_keys=True)
        return 0
    report(operations)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
