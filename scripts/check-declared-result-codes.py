#!/usr/bin/env python3
"""A handler may not answer with a status its catalog entry does not declare.

The catalog's `results` list is what a caller is told an operation can answer.
A handler returning something outside it is a contract break that no compiler
sees: the byte goes on the wire, the caller does not expect it, and what happens
next depends entirely on how that caller's switch was written.

FOUND BY A CONSUMER, NOT BY US. The session building peer messaging needed
server_session_get to distinguish "no such session" from "the store broke" --
absent is a fact to record, unreachable is a reason to retry, and choosing
wrong destroys mail. It read the catalog, saw ["ok", "invalid", "failed"], and
correctly concluded the operation could not say missing. The HANDLER had
returned StatusMissing all along. The behaviour was right and the declaration
was wrong, so a reader doing exactly the right thing got the wrong answer.

Its three siblings -- primary_session_load, webchat_claude_session_get,
webchat_live_get -- all declared missing. server_session_get was the exception,
and it was the one a session directory needs.

WHAT THIS COMPARES: the store's Go handlers, per operation, against that
operation's declared results. A handler's statuses are read from the function
the family's Ops table names for it.

WHAT IT CANNOT SEE: a status returned through a helper the handler calls. Those
read as no statuses at all rather than as wrong ones, so this under-reports
rather than inventing findings.
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FAMILIES = REPO_ROOT / "server-go" / "modules" / "aimee" / "families"
CATALOG = REPO_ROOT / "server-go" / "modules" / "aimee" / "operations.json"

# store.StatusOK -> "ok", and the transport-level failure a returned error
# becomes. An operation that returns a non-nil error is answered StatusFailed by
# Family.run, so "failed" is implied by any `return ..., err` path.
STATUS_WORD = {
    "StatusOK": "ok",
    "StatusMissing": "missing",
    "StatusInvalid": "invalid",
    "StatusTooLong": "too_long",
    "StatusFailed": "failed",
}


def handler_bodies() -> dict[str, str]:
    """Go function name -> its body text, across the families package."""
    bodies: dict[str, str] = {}
    for path in sorted(FAMILIES.glob("*.go")):
        if path.name.endswith("_test.go"):
            continue
        text = path.read_text()
        for match in re.finditer(r"(?m)^func (\w+)\(ctx context\.Context,", text):
            name = match.group(1)
            start = match.start()
            # To the next top-level func, which is where this one ends.
            nxt = text.find("\nfunc ", match.end())
            bodies[name] = text[start: nxt if nxt != -1 else len(text)]
    return bodies


def operation_handlers() -> dict[str, str]:
    """Catalog operation name -> the Go function its Ops entry names."""
    mapping: dict[str, str] = {}
    for path in sorted(FAMILIES.glob("*.go")):
        if path.name.endswith("_test.go"):
            continue
        for entry in re.finditer(
            r'\{\s*Name:\s*"(\w+)"[^}]*?(?:Run|RunDB):\s*(\w+)', path.read_text(), re.S
        ):
            mapping[entry.group(1)] = entry.group(2)
    return mapping


def main() -> int:
    catalog = json.loads(CATALOG.read_text())
    declared = {op["name"]: set(op.get("results") or []) for op in catalog["operations"]}
    if not declared:
        print("check-declared-result-codes: the catalog declares no operations")
        return 2

    bodies, handlers = handler_bodies(), operation_handlers()
    if not handlers:
        print("check-declared-result-codes: matched no operation to a handler; "
              "the Ops table shape moved and this would pass having compared nothing")
        return 2

    undeclared: list[tuple[str, str]] = []
    compared = 0
    for op, func in sorted(handlers.items()):
        if op not in declared or func not in bodies:
            continue
        body = bodies[func]
        returned = {word for const, word in STATUS_WORD.items()
                    if re.search(rf"\bstore\.{const}\b", body)}
        if re.search(r"return\s+0,\s*nil,\s*err\b", body):
            returned.add("failed")
        if not returned:
            continue
        compared += 1
        for status in sorted(returned - declared[op]):
            undeclared.append((op, status))

    if undeclared:
        for op, status in undeclared:
            print(f"    {op}: handler answers {status!r}, which its catalog entry "
                  f"does not declare")
        print(f"\n{len(undeclared)} undeclared status(es) across {compared} operations.")
        print("  A caller reads the catalog to know what an operation can say. A")
        print("  status outside that list is one nobody wrote a branch for.")
        return 1

    print(f"check-declared-result-codes: ok ({compared} operations compared)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
