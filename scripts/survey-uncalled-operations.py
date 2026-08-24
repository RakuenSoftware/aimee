#!/usr/bin/env python3
"""Store operations that no production caller invokes.

Asked because another session found its module shipped INERT: the capability was
built with a nil directory, nothing in production wrote the map that nil fell
back to, and fifteen green probe checks could not have detected it. A module
that correctly refuses an unregistered sender and a module that can never HAVE a
sender produce the same answer.

The transferable question is not "do the tests pass" but "what calls this in
production, and would the tests look any different if nothing did".

For the store that is a live question per operation. An operation with no caller
is not automatically wrong -- it may be a new surface, or one reached only from
a path this survey cannot see -- but it is the set worth looking at, because a
handler nothing calls is tested only by tests written for it, and those pass
whether or not anything could ever use it.

WHAT COUNTS AS A CALLER: the operation's generated C constant
(AIMEE_DB1_OP_<NAME>) appearing outside the client that defines it, or the
operation name appearing in Go outside the module that serves it.

WHAT IT FOUND, AND WHY THE ANSWER IS "NONE": two of 463 came back uncalled,
state_load and state_save, and both are false negatives. They are the
economizer's keyed-blob pair, the only operations that do not speak
db1-fields-v2, and their caller is server-go/db1's LoadState/SaveState using
NUMERIC ops on a separate wire rather than the catalog's names. Nothing this
survey searches for appears at that call site.

So the store has no inert operation. The strong evidence is not this survey
though -- it is scripts/check-db1-client-contract.py, which matches 461 real
call_stage sites against the catalog by arity and reply width. Those are
invocations rather than mentions. This survey adds the remaining two and
otherwise only proves nothing is unmentioned, which is a weaker claim: a name in
a comment counts here and should not.

Kept because the QUESTION is worth re-asking whenever the surface grows, and
because the two false negatives are the interesting part. An operation on a
second wire is exactly the one a name-based search cannot see, which is the same
reason a module with no directory and a module correctly refusing an unknown
sender give the same answer.
"""

import glob
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CATALOG = REPO_ROOT / "server-go" / "modules" / "aimee" / "operations.json"

# Where the client defines the constants; a definition is not a call.
DEFINING = {"db1_module_api.h"}


def callers() -> str:
    """Every text that could invoke an operation, concatenated."""
    body = []
    roots = [
        REPO_ROOT / "src",
        REPO_ROOT / "server-go",
        REPO_ROOT / "runtime-web",
        REPO_ROOT / "control-web",
    ]
    for root in roots:
        for suffix in ("*.c", "*.h", "*.go"):
            for path in root.rglob(suffix):
                if path.name in DEFINING:
                    continue
                # The module that SERVES an operation is not a caller of it.
                if "modules/aimee" in str(path):
                    continue
                try:
                    body.append(path.read_text(errors="ignore"))
                except OSError:
                    continue
    return "\n".join(body)


def main() -> int:
    if not CATALOG.exists():
        print(f"survey-uncalled-operations: no catalog at {CATALOG}")
        return 2
    catalog = json.loads(CATALOG.read_text())
    operations = [op["name"] for op in catalog.get("operations", [])]
    if not operations:
        print("survey-uncalled-operations: the catalog declares no operations")
        return 2

    text = callers()
    if not text:
        print("survey-uncalled-operations: read no sources; the layout moved")
        return 2

    uncalled = []
    for name in operations:
        constant = "AIMEE_DB1_OP_" + name.upper()
        if constant in text or re.search(rf"\b{re.escape(name)}\b", text):
            continue
        uncalled.append(name)

    for name in sorted(uncalled):
        print(f"    {name}")
    print(f"\n{len(uncalled)} of {len(operations)} operation(s) have no caller outside "
          f"the module that serves them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
