#!/usr/bin/env python3
"""Measure what the remaining DB1 operations need from the module wire.

Reports, for every DB1 function that a linked binary still calls: which wire
capabilities its signature still needs, and what its callers pass.

The T0..T3 ladder this script used to print has been retired, because three of
its four rungs shipped: integer arguments, the counted reply (which carried
multi-value replies and zero-argument requests with it), and struct flattening.
A ladder also modelled the remainder as if each rung sat on the one below, and
it does not -- a callee-allocated out-parameter and a richer return contract
are independent, and neither waits on the other.

What replaced it is a SET of missing capabilities per operation, unioned per
source. That matches how migration actually works: the unit that moves is a
whole .c file, so a source is ready exactly when that union is empty, and the
question worth asking is which single capability empties the most of them.

This is a measurement, not a gate. It reads the headers, the Makefile and the
call sites, so its numbers move when any of those move -- which is the point.
docs/proposals/pending/db1-wire-capability-survey.md records what it said on the
day it was written; re-run this before trusting those numbers again.

Four counting mistakes are deliberately designed out, because all four were
made, and every one of them flattered the migration:

  * Signatures are read from the HEADERS with line joins collapsed. Reading
    definitions line by line silently drops every wrapped signature.
  * CMD_SRCS is excluded. Those files are compiled by cmd-srcs-compile-check so
    they cannot rot, but they are linked into nothing, and counting them as
    production inflates the surface by about a fifth.
  * What remains is read from DB1_SRCS, not from which families are active. A
    family goes active to reserve its event kind and let its operations be
    declared; that moves no source out of the daemon. Treating activation as
    cutover hid seven sources and 23 operations in three active families.
  * A pointer return is matched without demanding whitespace after the type.
    "char *db1_wm_assemble_context(...)" binds the star to the name, and the
    old pattern skipped every declaration written that way -- which is to say
    most of the malloc-returning ones, the exact category it then reported as
    small. It found 24 alloc operations where it had claimed 10, and a source
    the survey had called ready failed to link.

A caveat this cannot design out: readiness is measured per SOURCE, but the
generated client is per FAMILY and links as one object. A source whose family
has any undeclared symbol still in the daemon cannot cut over alone, because
the client would define symbols the domain also defines. See the survey doc.
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

# A pointer return binds its star to the NAME as often as to the type --
# "char *db1_wm_assemble_context(...)" -- so a pattern demanding whitespace
# after the return type misses every one written that way. Which is precisely
# the malloc-returning functions, the category the survey then under-reports.
# Split so a value return still requires the space that separates it from the
# name, and a pointer return does not.
DECL = re.compile(
    r"\b(?:(?P<value>int|void|int64_t|long long|long|unsigned|double|size_t)\s+"
    r"|(?P<pointer>char\s*\*|const\s+char\s*\*)\s*)"
    r"(?P<name>[a-z][a-z0-9_]{3,})\s*\((?P<params>[^;{]*)\)\s*;", re.S)
CALL = re.compile(r"\b([a-z][a-z0-9_]{2,})\s*\(")

TEXT = re.compile(r"const char \s*\*\s*\w+$")
SCALAR = re.compile(r"(int|int64_t|long|long long|unsigned long long|unsigned int"
                    r"|unsigned|time_t|double) \w+$")
# An enum passed by value is an integer. A NON-enum _t by value would not be,
# so the enum names are read from the headers rather than assumed from the
# suffix -- most _t types here are structs.
ENUM_DECL = re.compile(r"typedef\s+enum\b[^;]*?\}\s*(\w+_t)\s*;", re.S)
BY_VALUE = re.compile(r"\b([a-z_0-9]+_t) \w+$")
# An array of fixed-width strings: rows of one text column, spelled as a
# pointer to an array rather than a pointer to a struct.
# Both spellings of the same thing: "char (*out)[N]" and "char out[][N]" are
# the same parameter type, and matching only the first made an ordinary text
# column look like a shape nobody had classified.
OUT_TEXT_ROWS = re.compile(r"char \s*(?:\(\s*\*\s*\w+\s*\)|\w+\s*\[\s*\])\s*\[")
# A variable-length list of strings as INPUT. The counted frame could carry
# it -- the terms would travel as N fields -- but every operation declares a
# fixed arity today, so it is a capability rather than a gap.
REPEATED_TEXT = re.compile(r"const char \s*\*\s*const \s*\*\s*\w+$")
JSON_IN = re.compile(r"(const )?cJSON \s*\*\s*\w+$")
OUT_TEXT = re.compile(r"char \s*\*\s*\w+$")
CAP = re.compile(r"size_t \w+$")
OUT_NUM = re.compile(r"(int|int64_t|double|size_t|long|long long"
                     r"|unsigned long long) \s*\*\s*\w+$")
STRUCT_IN = re.compile(r"const [a-z_0-9]+_t \s*\*\s*\w+$")
STRUCT_OUT = re.compile(r"([a-z_0-9]+_t) \s*\*\s*\w+$")
# Types whose "_t" is a scalar rather than a row.
SCALAR_TYPES = frozenset({"int64_t", "uint64_t", "int32_t", "size_t", "time_t"})
ARRAY_LEN = re.compile(r"(int|size_t) (max|n|count|cap|limit)\w*$")
# A callee-allocated out-parameter: the double star is the whole tell, and it is
# why these fall through STRUCT_OUT/OUT_TEXT, whose \w+$ cannot match a second *.
ALLOC_OUT = re.compile(r"(char|[a-z_0-9]+_t) \s*\*\s*\*\s*\w+$")
# Names that carry a prompt, a result or a document rather than an identifier.
LARGE = re.compile(r"(json|metadata|content|prompt|body|payload|text|summary|blob"
                   r"|result|output|detail|notes?|message|reason|sql|patch|diff)", re.I)

# What a tag still needs from the wire. A tag absent from this map needs
# nothing: text, int, out_text, out_num, struct_in and struct_out all cross
# today, the last two since struct flattening.
# Rows cross today, struct or column: a column is a list one value wide, and
# char (*out)[N] lands straight in the caller's fixed-width row. The NUMERIC
# column -- int64_t *out -- is the one that does not, only because no family
# that can be activated yet has one to prove it against.
NEEDS = {
    "repeated_text": "repeated",
    "alloc_out": "alloc",
    "json_in": "json",
    "other": "unknown",
}
CAPABILITY = {
    "repeated": "repeated -- a variable-length list of strings as an argument",
    "alloc": "alloc -- a callee-allocated out-parameter, T ** or char **",
    "json": "json -- a cJSON tree, which the wire carries but the client must build",
    "status": "status -- a return contract beyond ok/miss/fail, per operation",
    "unknown": "unknown -- a parameter shape the classifier does not recognise",
    "member": "member -- a reply struct carries a member type the wire has no field for",
}

# A struct crosses as its members, so a member the wire has no type for blocks
# the operation just as surely as an unsupported parameter would. The survey
# classified PARAMETERS only and never looked inside the struct a parameter
# points at, so an operation taking a perfectly ordinary "T *out" reported
# ready while T carried a double. Twenty-eight structs do.
STRUCT_BODIES = re.compile(r"typedef\s+struct\s*\{(.*?)\}\s*([a-z_0-9]+_t)\s*;", re.S)
UNSUPPORTED_MEMBER = re.compile(r"^\s*(float)\b")


def struct_members(root: Path) -> dict[str, list[str]]:
    """Member type names per reply struct, for the types the wire must carry."""
    found: dict[str, list[str]] = {}
    for header in sorted((root / SOURCE_DIR).glob("*.h")):
        for body, name in STRUCT_BODIES.findall(header.read_text(errors="ignore")):
            kinds = []
            for member in body.split(";"):
                member = re.sub(r"/\*.*?\*/", "", member, flags=re.S).strip()
                if UNSUPPORTED_MEMBER.match(member):
                    kinds.append(member.split()[0])
            found[name] = kinds
    return found


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


def remaining_sources(root: Path) -> dict[str, str]:
    """Sources the daemon still links, whatever their family's state.

    This used to select families that were not yet active, and that was wrong
    in the direction that flatters the migration. Activating a family reserves
    an event kind and lets its operations be declared; it does not move a
    single source out of the daemon. Cutover does, by dropping the .c from
    DB1_SRCS -- which is exactly what this reads. Seven sources across three
    ACTIVE families were invisible under the old rule, including the one whose
    client had been generated but not yet linked.
    """
    catalog = json.loads((root / CATALOG).read_text(encoding="utf-8"))
    makefile = (root / MAKEFILE).read_text(encoding="utf-8")
    block = re.search(r"^DB1_SRCS\s*[:+]?=(.*?)(?=^\w)", makefile, re.M | re.S)
    linked = {Path(s).name for s in re.findall(r"([A-Za-z0-9_/.\-]+)\.c",
                                               block.group(1) if block else "")}
    return {source: family["name"]
            for family in catalog["families"]
            for source in family["sources"] if source in linked}


# re.S as well as re.M: a definition whose parameter list wraps is still a
# definition. This is the same wrapped-signature mistake the docstring above
# says was designed out for DECLARATIONS, reintroduced here for definitions --
# it hid seven db1_cron_job_* symbols until the unattributed report named them.
DEFINITION = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_ \*]*?\b([a-z][a-z0-9_]{3,})\s*\([^;]*?\)\s*\{", re.M | re.S)


def defining_sources(root: Path) -> dict[str, str]:
    """Which .c file defines each symbol.

    Attribution used to go by FILENAME: a declaration counted only if its
    header's stem matched a claimed source name. windows.c declares its API in
    db1_windows.h and db1_cron_jobs.c in cron_jobs.h, so every operation in both
    was invisible -- not classified as blocked, not counted at all. A symbol
    belongs to whichever source defines it, which is a fact rather than a naming
    convention.
    """
    owner: dict[str, str] = {}
    for source in sorted((root / SOURCE_DIR).glob("*.c")):
        text = re.sub(r"/\*.*?\*/", "", source.read_text(errors="ignore"), flags=re.S)
        for symbol in DEFINITION.findall(text):
            owner.setdefault(symbol, source.stem)
    return owner


def declarations(root: Path, families: dict[str, str]) -> dict[str, dict]:
    found: dict[str, dict] = {}
    unattributed: list[str] = []
    owner = defining_sources(root)
    for header in sorted((root / SOURCE_DIR).glob("*.h")):
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
            returns = match.group("value") or match.group("pointer")
            source = owner.get(match.group("name"))
            if source is None:
                # A declaration whose definition is in no DB1 .c file: an
                # inline, a macro, or a symbol implemented elsewhere. Recorded
                # rather than dropped, because "the survey silently skipped it"
                # is exactly how windows.c stayed invisible.
                unattributed.append(match.group("name"))
                continue
            if source not in families:
                continue
            found[match.group("name")] = {
                "family": families[source],
                "source": source,
                "returns": " ".join(returns.split()),
                "params": " ".join(match.group("params").split()),
                "contract": " ".join(documented.get(match.group("name"), "").split()),
            }
    if unattributed:
        print(f"survey: {len(unattributed)} declaration(s) define nothing in "
              f"{SOURCE_DIR} and were not counted: "
              f"{', '.join(sorted(unattributed)[:6])}"
              f"{' ...' if len(unattributed) > 6 else ''}", file=sys.stderr)
    return found


def enum_types(root: Path) -> set[str]:
    names: set[str] = set()
    for header in sorted((root / SOURCE_DIR).glob("*.h")):
        names.update(ENUM_DECL.findall(header.read_text(errors="ignore")))
    return names


def classify(params: str, enums: frozenset[str] = frozenset()) -> list[str]:
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
            # int64_t matches the _t pattern too, so a column of integers would
            # otherwise be counted as a struct it has no members for.
            row = STRUCT_OUT.search(current).group(1)
            tags.append("out_column" if row in SCALAR_TYPES else "out_rows")
            index += 2
            continue
        if following and OUT_TEXT_ROWS.search(current) and ARRAY_LEN.search(following):
            tags.append("out_rows")
            index += 2
            continue
        if OUT_TEXT_ROWS.search(current):
            tags.append("out_rows")
        elif JSON_IN.search(current):
            tags.append("json_in")
        elif REPEATED_TEXT.search(current):
            tags.append("repeated_text")
        elif ALLOC_OUT.search(current):
            tags.append("alloc_out")
        elif TEXT.search(current):
            tags.append("text")
        elif SCALAR.search(current):
            tags.append("int")
        elif STRUCT_IN.search(current):
            tags.append("struct_in")
        elif OUT_NUM.search(current):
            tags.append("out_num")
        elif STRUCT_OUT.search(current) or OUT_TEXT.search(current):
            tags.append("struct_out")
        elif BY_VALUE.search(current) and BY_VALUE.search(current).group(1) in enums:
            tags.append("int")
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
    families = remaining_sources(root)
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
    enums = frozenset(enum_types(root))
    members = struct_members(root)
    for name, sites in callers.items():
        entry = dict(declared[name], callers=sorted(sites))
        entry["tags"] = classify(entry["params"], enums)
        needs = {NEEDS[tag] for tag in entry["tags"] if tag in NEEDS}
        # A returned string crosses today: the client allocates the declared
        # maximum, the stage hands the domain's own string to the reply, and
        # the caller frees what it is given exactly as before. A callee
        # allocated OUT-PARAMETER still does not -- that is the alloc_out tag.
        # A return contract that says more than success/failure or found/not
        # -- a count, or a distinguished refusal like the single-writer -2 --
        # has nowhere to put the distinction it exists to make. The counted
        # reply gives it somewhere; what the value MEANS is still a decision
        # per operation, which is why this stays a blocker and not a feature.
        if RICH_RETURN.search(entry.get("contract", "")):
            needs.add("status")
        # A struct crosses as its members. An operation whose row carries a
        # member type the wire cannot spell is blocked by that, however
        # ordinary its own parameter list looks.
        for struct in re.findall(r"\b([a-z_0-9]+_t)\b", entry["params"]):
            if members.get(struct):
                needs.add("member")
                entry["blocking_members"] = sorted(set(members[struct]))
        # Zero arguments and a second out buffer were both blockers under the
        # single-value reply. The counted reply carried them: a request may
        # declare no fields, and a reply may carry as many as it needs.
        entry["needs"] = sorted(needs)
        names = [p.split()[-1].lstrip("*") for p in entry["params"].split(",")]
        entry["large_fields"] = sorted({
            field for tag, field in zip(entry["tags"], names)
            if tag == "text" and LARGE.search(field)})
        operations[name] = entry

    # Nullability, for the operations that could migrate today.
    for name, entry in operations.items():
        if entry["needs"]:
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
    print(f"DB1 operations with a linked caller: {total}")
    print(f"caller files involved              : "
          f"{len({c for o in operations.values() for c in o['callers']})}\n")
    blocked = collections.Counter(
        need for o in operations.values() for need in o["needs"])
    fits = sum(1 for o in operations.values() if not o["needs"])
    print(f"  {'fits the wire today':62} {fits:>4} ({100 * fits // total}%)")
    for need, count in blocked.most_common():
        print(f"  needs {CAPABILITY[need]:56} {count:>4}")

    ready = {n: o for n, o in operations.items() if not o["needs"]}
    nullable = {n: o for n, o in ready.items() if o["nullable"]}
    large = {n: o for n, o in ready.items() if o["large_fields"]}
    print(f"\n  reachable now                        : {len(ready)}")
    # Both of these were blockers once and are not now. They stay in the report
    # because they say what the reachable set is carrying -- a family full of
    # documents is a different migration from a family full of identifiers --
    # but neither refuses a call any more.
    print(f"    passing NULL or \"\" somewhere      : {len(nullable)}"
          f"   (carried: fields declare required)")
    print(f"    carrying a prompt/result/document  : {len(large)}"
          f"   (carried: requests are not capped)")

    # The unit that can actually migrate is a whole DB1 source, not an
    # operation: the daemon swaps a .c file out of its link, so a source with
    # one operation the wire cannot express keeps ALL of its operations
    # in-process -- the client and the domain would otherwise both define the
    # symbols that did move. Per-operation readiness overstates the work that
    # can be done by roughly six times.
    per_source = collections.defaultdict(set)
    source_ops = collections.Counter()
    for entry in operations.values():
        per_source[entry["source"]].update(entry["needs"])
        source_ops[entry["source"]] += 1
    done = [s for s, needs in per_source.items() if not needs]
    print("\n  whole sources ready to migrate (the unit that can actually move):")
    print(f"    today     {len(done):>2} of {len(per_source)} sources"
          f"   {sum(source_ops[s] for s in done):>3} of {total} operations")

    # The question this instrument exists to answer: which ONE capability
    # empties the most sources. A source needing two is unlocked by neither
    # alone, so these do not sum -- the combined row is what they buy together.
    print("\n  sources unlocked by adding one capability:")
    for need in sorted(CAPABILITY):
        gained = [s for s, n in per_source.items() if n and n <= {need}]
        if not gained:
            continue
        print(f"    + {need:9} {len(gained):>2} more sources"
              f"   {sum(source_ops[s] for s in gained):>3} operations")
    for combo in ("column alloc", "column alloc json status"):
        allowed = set(combo.split())
        gained = [s for s, n in per_source.items() if n and n <= allowed]
        print(f"    + {combo:17} {len(gained):>2} more sources"
              f"   {sum(source_ops[s] for s in gained):>3} operations")

    print("\n  by family (ready / total, then what the rest still need):")
    families = collections.defaultdict(collections.Counter)
    family_needs = collections.defaultdict(collections.Counter)
    for entry in operations.values():
        families[entry["family"]]["ready" if not entry["needs"] else "blocked"] += 1
        for need in entry["needs"]:
            family_needs[entry["family"]][need] += 1
    for family in sorted(families, key=lambda f: -sum(families[f].values())):
        counts = families[family]
        rest = "  ".join(f"{need} {n}" for need, n in
                         sorted(family_needs[family].items()))
        print(f"    {family:14} {counts['ready']:>3} / "
              f"{sum(counts.values()):<3}   {rest}")


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
