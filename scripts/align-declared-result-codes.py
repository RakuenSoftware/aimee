#!/usr/bin/env python3
"""Make each operation's declared results match what its handler answers.

One-shot repair for what check-declared-result-codes.py finds. The catalog
documents behaviour and the handlers ARE the behaviour, so the declaration is
the side that moves.

Additions are derived from the handler bodies rather than chosen, and the
checker refuses anything left over, so this cannot quietly declare a status the
code does not produce.
"""

import collections
import importlib.util
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CATALOG = REPO_ROOT / "server-go" / "modules" / "aimee" / "operations.json"

# The order a reader expects: success, absence, refusal, size, failure.
ORDER = ["ok", "missing", "invalid", "too_long", "failed"]


def checker():
    spec = importlib.util.spec_from_file_location(
        "declared_result_codes", REPO_ROOT / "scripts" / "check-declared-result-codes.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    chk = checker()
    bodies, handlers = chk.handler_bodies(), chk.operation_handlers()
    if not handlers:
        print("align-declared-result-codes: matched no operation to a handler")
        return 2

    catalog = json.loads(CATALOG.read_text(), object_pairs_hook=collections.OrderedDict)
    changed = []
    for op in catalog["operations"]:
        func = handlers.get(op["name"])
        if not func or func not in bodies:
            continue
        body = bodies[func]
        returned = {word for const, word in chk.STATUS_WORD.items()
                    if re.search(rf"\bstore\.{const}\b", body)}
        if re.search(r"return\s+0,\s*nil,\s*err\b", body):
            returned.add("failed")
        if not returned:
            continue
        declared = set(op.get("results") or [])
        if not returned - declared:
            continue
        before = list(op.get("results") or [])
        op["results"] = [s for s in ORDER if s in (declared | returned)]
        changed.append((op["name"], before, op["results"]))

    CATALOG.write_text(json.dumps(catalog, indent=2) + "\n")
    for name, before, after in changed[:8]:
        print(f"  {name}: {before} -> {after}")
    if len(changed) > 8:
        print(f"  ... and {len(changed) - 8} more")
    print(f"\n{len(changed)} operation(s) now declare what their handler answers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
