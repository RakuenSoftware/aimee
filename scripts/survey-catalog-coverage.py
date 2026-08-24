#!/usr/bin/env python3
"""Every catalog operation must have a handler, and every handler an entry.

The catalog is what 461 C call sites compile against. An operation declared
there with no handler is a call the daemon routes and the module refuses; a
handler with no catalog entry is an operation nobody can discover and nothing
validates the shape of.

Neither fails a build. The Ops tables are Go maps and the catalog is JSON, and
nothing has compared their key sets.
"""

import glob
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CATALOG = REPO_ROOT / "server-go" / "modules" / "aimee" / "operations.json"
FAMILIES = REPO_ROOT / "server-go" / "modules" / "aimee" / "families"


def main() -> int:
    catalog = json.loads(CATALOG.read_text())
    declared = {op["name"] for op in catalog["operations"]}
    if not declared:
        print("survey-catalog-coverage: the catalog declares no operations")
        return 2

    implemented: set[str] = set()
    for path in sorted(FAMILIES.glob("*.go")):
        if path.name.endswith("_test.go"):
            continue
        text = path.read_text()
        # Ops entries only, and the first pattern got both halves wrong.
        #
        # It required `{` immediately before `Name:`, which misses the shape
        # where a comment explains an unusual operation first:
        #
        #     opModelCatalogReplace: {
        #         // ... why Args is -1 ...
        #         Name: "model_catalog_replace", Args: -1, Run: modelCatalogReplace,
        #
        # model_catalog_replace read as UNIMPLEMENTED while sitting in the table
        # with a Run function. And a Family's own Name field matches the same
        # shape, so nineteen family names read as operations with no catalog
        # entry.
        #
        # What separates them: an Op names a Run or RunDB. A Family never does.
        for entry in re.finditer(r'Name:\s*"(\w+)"[^{}]*?(?:Run|RunDB):\s*\w+', text, re.S):
            implemented.add(entry.group(1))

    # The keyed-blob pair is served through Bind rather than an Ops table: two
    # operations speak their own codec and carry their own handler. Counting
    # only the fields-v2 path reports them as unimplemented, which is the same
    # mistake the process-contract test made before it learned about Bind. A
    # caller cannot tell the two dispatch paths apart and neither should this.
    for name in ("state_load", "state_save"):
        if re.search(rf'op{name.title().replace("_", "")}\b',
                     (FAMILIES / "economizer_state.go").read_text()):
            implemented.add(name)
    if not implemented:
        print("survey-catalog-coverage: found no Ops entries; the table shape moved")
        return 2

    unhandled = sorted(declared - implemented)
    undeclared = sorted(implemented - declared)

    print(f"catalog declares {len(declared)}, families implement {len(implemented)}")
    if unhandled:
        print(f"\ndeclared with no handler ({len(unhandled)}):")
        for name in unhandled:
            print(f"    {name}")
    if undeclared:
        print(f"\nimplemented with no catalog entry ({len(undeclared)}):")
        for name in undeclared:
            print(f"    {name}")
    if not unhandled and not undeclared:
        print("every operation has a handler and every handler an entry")
    return 0


if __name__ == "__main__":
    sys.exit(main())
