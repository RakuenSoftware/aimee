"""Which catalogued DB2 operations the Go module implements, and which it does not.

The mapping is taken from the generated Go contract rather than derived from the
catalogue name. Deriving it needs a name-casing rule ("l2_memory_ids" ->
"L2MemoryIDs", not "L2MemoryIds"), and getting that rule slightly wrong reports
an implemented operation as missing -- which is how the port came to be counted
against 307 operations when the catalogue declares 445.
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

CONTRACT = Path("server-go/db2/contract_generated.go")
CATALOG = Path("src/modules/db2/eventcontract/operations.json")
MODULE = Path("server-go/modules/db2")


def contract_operations() -> dict:
    """(stage id, operation id) -> Go constant name, from the contract itself."""
    text = CONTRACT.read_text()
    stages = dict(re.findall(r"^const Stage(\w+) = Family(\w+)$", text, re.M))
    operations = dict(
        (name, int(value))
        for name, value in re.findall(r"^const Operation(\w+) uint32 = (\d+)$", text, re.M)
    )
    families = dict(
        (name, int(value))
        for name, value in re.findall(r"^const Family(\w+) uint32 = (\d+)$", text, re.M)
    )
    resolved = {}
    for name, family in stages.items():
        if name in operations and family in families:
            resolved[(families[family], operations[name])] = name
    return resolved


def registered() -> set:
    names = set()
    for path in MODULE.glob("*.go"):
        if path.name.endswith("_test.go"):
            continue
        text = path.read_text()
        # Either constant counts. Health is served by the dispatcher's own
        # branch rather than by a registry entry, so it names only the stage.
        names |= set(re.findall(r"db2contract\.Operation(\w+)\b", text))
        names |= set(re.findall(r"db2contract\.Stage(\w+)\b", text))
    return names


def main() -> int:
    catalog = json.loads(CATALOG.read_text())
    stage_of = {f["name"]: int(f["id"]) for f in catalog["families"]}
    by_key = contract_operations()
    have = registered()

    missing, implemented, unmapped = [], 0, []
    for op in catalog["operations"]:
        key = (stage_of.get(op["family"]), int(op["id"]))
        name = by_key.get(key)
        if name is None:
            unmapped.append(f"{op['family']}.{op['name']}")
            continue
        if name in have:
            implemented += 1
        else:
            missing.append((op["family"], op["name"], name, op["wire_format"]))

    total = len(catalog["operations"])
    print(f"catalogued {total}, implemented {implemented}, missing {len(missing)}")
    if unmapped:
        print(f"  {len(unmapped)} could not be mapped to a contract constant: "
              f"{unmapped[:5]}")
    if missing:
        print("  by family:", dict(Counter(family for family, _, _, _ in missing)))
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    for family, name, constant, wire in missing:
        if want and family != want:
            continue
        print(f"   {family:13s} {name:44s} {constant:44s} {wire}")
    return 0 if not missing else 1


if __name__ == "__main__":
    raise SystemExit(main())
