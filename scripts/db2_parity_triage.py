"""Group a parity report by cause and name the operations in it.

The parity test writes one divergence per line as

    <trace line>\t<stage>\t<operation>\t<kind>\t<detail>

which is deliberately numeric: the Go module knows stage and operation ids and
nothing about catalogue names. This maps them back, so a run says which
operations disagree rather than which numbers do -- and groups them, because 500
calls produce more divergences than anyone reads one at a time.
"""
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

CATALOG = Path("src/modules/db2/eventcontract/operations.json")
# The stage id is the family id the catalogue declares, and the operation id is
# unique only within its family -- which is why both are carried.


def load_names() -> dict:
    catalog = json.loads(CATALOG.read_text())
    stage_of = {family["name"]: int(family["id"]) for family in catalog["families"]}
    names = {}
    for op in catalog["operations"]:
        stage = stage_of.get(op["family"])
        if stage is None:
            continue
        names[(stage, int(op["id"]))] = (op["family"], op["name"], op["wire_format"])
    return names


def main() -> int:
    report = Path(sys.argv[1] if len(sys.argv) > 1 else "parity-report.tsv")
    if not report.exists():
        print(f"no report at {report}")
        return 1
    names = load_names()
    kinds = Counter()
    by_kind = defaultdict(list)
    unnamed = 0
    for line in report.read_text().splitlines():
        if not line.strip():
            continue
        _, stage, operation, kind, detail = line.split("\t", 4)
        key = (int(stage), int(operation))
        named = names.get(key)
        if named is None:
            unnamed += 1
            named = ("?", f"stage{stage}/op{operation}", "?")
        kinds[kind] += 1
        by_kind[kind].append((named, detail))

    print(f"{sum(kinds.values())} divergences: {dict(kinds)}")
    if unnamed:
        print(f"  ({unnamed} could not be named from the catalogue)")
    for kind, rows in sorted(by_kind.items()):
        print(f"\n=== {kind} ({len(rows)}) ===")
        seen = Counter()
        for (family, name, wire), detail in rows:
            seen[(family, name, wire, detail.split(",")[0])] += 1
        for (family, name, wire, detail), count in seen.most_common(40):
            times = f" x{count}" if count > 1 else ""
            print(f"   {family:13s} {name:42s} {wire:44s} {detail}{times}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
