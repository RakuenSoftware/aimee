#!/usr/bin/env python3
"""Every Go file in a module tree must be claimed by a module descriptor.

NAMED FOR WHAT IT CHECKS. scripts/check_module_source_ownership.py already
exists and asks a different question -- it enforces the C legacy source/header
contracts for landed modularization slices. This one asks whether every .go
file under server-go/modules is listed in some descriptor's go_sources. Two
checks, two questions; near-identical names were a worse problem than either.

WHAT THIS CATCHES. A package under server-go/modules/ is, by its location, a
module: it serves stages, it owns state, it answers on event kinds. Its identity
comes from a descriptor -- src/modules/<id>/module.yaml -- which is what gives
it an id, its dependency edges, and the principal ref that the kind formula
turns into event kinds. A file that no descriptor lists has none of that. It
compiles, it links, it runs, and by the formula kind = 4096 + ref*256 + stage it
has no identity at all, because it has no ref.

WHY IT WAS WORTH WRITING. Nothing rejected that. Every other validator passes
with an undeclared module package present, because an undeclared package owning
server-side state is invisible rather than illegal -- the descriptors are read
to find what to build, so a file nobody declared is simply never looked at. The
gap was found by a session that had placed a new feature outside any module and
watched all three validators go green.

It was not hypothetical. db1's own descriptor declared 47 files under families/
and none of the five at the top of the tree -- its dispatch layer, its wire, and
the whole store client -- while asserting "ownership_complete": true.

Runs from anywhere. Exits non-zero on an unclaimed file.
"""

import json
import os
import sys
from pathlib import Path

# Resolved from this file's own location rather than the working directory: the
# Makefile target that runs this lives in src/, so a relative path would be
# looking one level down from where the trees actually are.
REPO_ROOT = Path(__file__).resolve().parent.parent
MODULE_TREE = REPO_ROOT / "server-go" / "modules"
DESCRIPTORS = REPO_ROOT / "src" / "modules"

# Trees exempt only while a stated condition still holds.
#
# A flat exemption list is a trap: it outlives the reason it was added, and the
# module most likely to churn becomes the one place the check never looks. This
# is not hypothetical -- the entry below was written against a tree where db2's
# descriptor declared nothing, while in another worktree it already declared 97
# sources. A list would have gone on skipping it after the reason evaporated.
#
# So an entry here is not "skip this tree". It is an ASSERTION about why the gap
# exists, re-tested on every run against the descriptor on disk. When the
# assertion stops holding the exemption retires itself and the tree is enforced
# like any other, with nobody having to remember.
#
# Removing an entry is the fix. Adding one requires a condition that can go
# false on its own: an exemption that cannot expire does not belong here.


def exemption(tree: str) -> str | None:
    """Say why `tree` is exempt right now, or None if it must be enforced."""
    descriptor = DESCRIPTORS / tree / "module.yaml"

    if tree == "db2":
        # Exempt only while the descriptor declares nothing at all. That is a
        # descriptor nobody has filled in, which is a different defect from a
        # file slipping out of a maintained list. db2 is mid-move under the
        # ruling -- domain operations leaving for control-plane, memory and
        # aimee, keeping a tree while that happens -- and a transitional state
        # is exactly when this check earns its keep, so the moment the
        # descriptor declares anything, every file in the tree is enforced.
        if not descriptor.exists():
            return None
        try:
            declared = json.loads(descriptor.read_text()).get("go_sources", [])
        except (OSError, json.JSONDecodeError):
            return None  # Unreadable: enforce and let the failure say so.
        if declared:
            return None
        return (
            "descriptor declares no Go sources at all; mid-move under the "
            "ruling. Enforced automatically once it declares any"
        )

    if tree == "mcp":
        # Exempt only while there is no descriptor. Writing one means allocating
        # a principal ref and choosing dependency edges -- an architectural
        # decision, not a lint's to make. The day the file appears, this is
        # enforced.
        if descriptor.exists():
            return None
        return (
            "no descriptor exists, so no principal ref and no event kinds; "
            "claiming it is an architectural decision"
        )

    return None


def declared_sources() -> dict[str, str]:
    """Map each declared source path to the descriptor that claims it."""
    claimed: dict[str, str] = {}
    for path in sorted(DESCRIPTORS.glob("*/module.yaml")):
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as err:
            print(f"check-module-descriptor-sources: cannot read {path}: {err}")
            raise SystemExit(2)
        for src in data.get("go_sources", []):
            if src in claimed:
                # Two descriptors claiming one file means two modules believe
                # they own it, and the build would follow whichever it read
                # last.
                print(
                    f"check-module-descriptor-sources: {src} is claimed by both "
                    f"{claimed[src]} and {path}"
                )
                raise SystemExit(1)
            claimed[src] = str(path)
    return claimed


def main() -> int:
    if not MODULE_TREE.is_dir():
        print(f"check-module-descriptor-sources: {MODULE_TREE} not found")
        return 2

    claimed = declared_sources()
    unclaimed: dict[str, list[str]] = {}
    walked = 0

    for root, _dirs, files in os.walk(MODULE_TREE):
        for name in sorted(files):
            if not name.endswith(".go") or name.endswith("_test.go"):
                continue
            walked += 1
            path = str(Path(os.path.join(root, name)).relative_to(REPO_ROOT))
            if path in claimed:
                continue
            # server-go/modules/<tree>/...
            tree = Path(path).parts[2]
            unclaimed.setdefault(tree, []).append(path)

    # Checked FIRST, before the exemption rules read anything into the
    # results: an empty tree makes every exemption look like it covers
    # nothing, which is a true statement and the wrong diagnosis.
    # The tree existing is not the same as it holding anything. `total` counts
    # what the DESCRIPTORS claim, so without this a moved module tree reports a
    # healthy number having walked no files at all -- the count would describe
    # the paperwork rather than the code.
    if walked == 0:
        print(f"check-module-descriptor-sources: no non-test sources under "
              f"{MODULE_TREE}; this check would pass having walked nothing")
        return 2

    failures = 0
    exempted = 0

    # An exemption covering nothing is itself a failure.
    #
    # The conditional exemptions above retire when their stated reason stops
    # holding. That is not sufficient on its own: a reason can stay true while
    # the files it covered move out from under it, and debt that has silently
    # relocated still reads as covered. So an exemption that matches no
    # unclaimed file has to be removed rather than left standing -- otherwise an
    # allowlist rots into a place anything can later hide. (Taken from the
    # session building the aimee module, whose complementary lint had the rule
    # first.)
    for tree in ("db2", "mcp"):
        if exemption(tree) is not None and tree not in unclaimed:
            print(
                f"check-module-descriptor-sources: {tree} is exempt but has no "
                f"unclaimed files -- the exemption covers nothing and must be "
                f"removed, or it becomes a place a later file can hide"
            )
            failures += 1

    for tree, paths in sorted(unclaimed.items()):
        why = exemption(tree)
        if why is not None:
            exempted += 1
            print(
                f"check-module-descriptor-sources: EXEMPT while true -- {tree} "
                f"({len(paths)} files): {why}"
            )
            continue
        failures += len(paths)
        print(f"check-module-descriptor-sources: no descriptor claims these files in {tree}:")
        for p in paths:
            print(f"    {p}")

    if failures:
        print(
            f"\n{failures} unclaimed file(s). A module package needs a "
            f"src/modules/<id>/module.yaml listing it in go_sources -- without "
            f"one it has no id, no dependency edges, and no principal ref, so "
            f"no event kinds and no identity."
        )
        return 1

    total = sum(1 for p in claimed if p.startswith("server-go/modules"))
    print(
        f"check-module-descriptor-sources: ok ({total} files claimed, "
        f"{walked} walked, {exempted} tree(s) currently exempt)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
