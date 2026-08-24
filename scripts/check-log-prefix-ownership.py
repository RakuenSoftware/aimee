#!/usr/bin/env python3
"""A module's diagnostics must not claim to come from a different module.

WHAT THIS CATCHES. Error and log strings in this tree are prefixed with the
emitting module's name -- "aimee: state load failed". An operator reading a log
uses that prefix to decide which process to look at, so a module emitting
another module's name sends them to the wrong one.

WHY IT HAPPENED. The store module's code lived at server-go/modules/postgres
before it moved, and 28 of its strings still said "postgres:" afterwards. That
was not merely stale: `postgres` is a REAL, DIFFERENT module (principal ref 28,
the health probe and the SQL stage), and it is precisely the process an operator
would suspect when storage misbehaves. The prefix pointed at the most plausible
wrong answer.

A rename moves the code and the compiler checks every reference to it. Nothing
checks the strings, because a string is correct as far as the compiler is
concerned no matter what it claims.

WHAT IT DOES NOT CHECK. That a module's strings are prefixed at all, or that the
prefix is well-formed. Only that a prefix which names a REAL OTHER MODULE is not
used from inside a module that is not it. Adding a "must be prefixed" rule would
be a much larger change to unrelated code and is not this script's business.

SUB-PACKAGE PREFIXES ARE FINE AND DELIBERATELY SO. A string saying "peer: ..."
inside the aimee module passes, because no module is called `peer` -- it names
which part of aimee spoke, which is more useful to an operator than a uniform
module prefix on everything. The rule is "does not name a DIFFERENT MODULE", not
"equals the owning directory".

The consequence, so it is not a surprise: if a module named `peer` is ever
declared, every "peer: " prefix inside another module starts failing. That is
CORRECT rather than a regression -- the prefix genuinely became ambiguous the
moment a real module claimed the name, and an operator reading it would now be
sent to the wrong process. The fix then is to rename the prefix, or, if the
usage really is unambiguous in context, to add it to AMBIGUOUS with the reason.

Run from anywhere. Exits non-zero on a misattributed prefix.
"""

import json
import re
import sys
from pathlib import Path

from source_text import strip_comments

REPO_ROOT = Path(__file__).resolve().parent.parent
# Scoped to modules/ because that is where the OWNER can be read off the path:
# server-go/modules/<owner>/... is what makes "this file emitted that prefix"
# an attribution rather than a guess.
#
# Widening to all of server-go was measured and would fire zero times today
# across 47 further files -- but that is not a reason to do it. Zero hits says
# the rule does not currently misfire there; it says nothing about whether the
# premise holds, and outside modules/ it does not: server-go/bus is a package,
# not a module with a principal ref, so there is no owner for a prefix to
# disagree with. Extending a rule into a directory where its premise is absent
# buys coverage that cannot mean anything.
#
# (The neighbouring check-memory-store-degradation reaches the OPPOSITE
# conclusion about its own scope for the opposite reason -- widening it would
# report 406 files. The question has to be asked per guard; neither answer
# generalises.)
MODULE_TREE = REPO_ROOT / "server-go" / "modules"
INVENTORY = REPO_ROOT / "tests/baselines/modules/canonical-inventory.yaml"

# A prefix at the start of a Go string literal: "name: ...".
PREFIX = re.compile(r'"([a-z][a-z0-9-]{2,}): ')
# Names that are module ids but are also ordinary words in a diagnostic, where
# "postgres: could not connect" from any module is describing the DATABASE
# rather than claiming to be the postgres module. Listed rather than inferred,
# because the distinction is about meaning and cannot be read off the string.
AMBIGUOUS = {
    # No entries. Kept as the documented place to put one, so a future
    # exception is a considered edit rather than a weakened regex.
}


def module_ids() -> set[str]:
    """Every declared module id, from the canonical inventory."""
    data = json.loads(INVENTORY.read_text())
    return set(data.get("required", [])) | set(data.get("optional", []))


def main() -> int:
    if not MODULE_TREE.is_dir():
        print(f"check-log-prefix-ownership: {MODULE_TREE} not found")
        return 2

    ids = module_ids()
    if not ids:
        print("check-log-prefix-ownership: the inventory declares no modules")
        return 2

    findings: list[tuple[str, int, str, str]] = []
    scanned = 0
    for path in sorted(MODULE_TREE.rglob("*.go")):
        if path.name.endswith("_test.go"):
            continue
        scanned += 1
        # server-go/modules/<owner>/...
        owner = path.relative_to(MODULE_TREE).parts[0]
        in_block = False
        for lineno, raw in enumerate(path.read_text().splitlines(), start=1):
            line, in_block = strip_comments(raw, in_block)
            for claimed in PREFIX.findall(line):
                if claimed == owner or claimed in AMBIGUOUS:
                    continue
                if claimed in ids:
                    findings.append(
                        (str(path.relative_to(REPO_ROOT)), lineno, owner, claimed))

    if findings:
        print("check-log-prefix-ownership: diagnostics naming another module:")
        for path, lineno, owner, claimed in findings:
            print(f"    {path}:{lineno}: module {owner!r} emits a {claimed!r} prefix")
        print(
            f"\n{len(findings)} misattributed prefix(es). An operator reads the "
            f"prefix to choose which process to investigate, so naming another "
            f"module points them at the wrong one."
            f"\n"
            f"\n  Fix the STRING, not this check. Adding the name to AMBIGUOUS"
            f"\n  silences the finding and leaves the operator being sent to the"
            f"\n  wrong process, which is the whole harm."
            f"\n"
            f"\n  AMBIGUOUS is for a name that is also an ordinary word -- a"
            f"\n  \"postgres: could not connect\" that describes the DATABASE rather"
            f"\n  than claiming to be the postgres module. That is a judgement about"
            f"\n  meaning, not a way to make a real misattribution go quiet."
        )
        return 1

    # The directory existing is not the same as it containing anything. Without
    # this, a moved module tree reports "ok (30 module names checked)" having
    # read no source at all -- the count names what it was WILLING to check, not
    # what it did.
    if scanned == 0:
        print(f"check-log-prefix-ownership: no non-test sources under {MODULE_TREE}; "
              f"this check would pass having read nothing")
        return 2

    print(f"check-log-prefix-ownership: ok ({scanned} files, {len(ids)} module names)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
