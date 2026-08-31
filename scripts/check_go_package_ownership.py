#!/usr/bin/env python3
"""Every Go package under server-go/ is owned by a module, or named here as not.

WHY THIS EXISTS
---------------
Peer messaging was first written into `server-go/internal/peer/` — a package
owning a session directory, message inboxes and an authorization table. It had
no module.yaml, so no id, no dependency edges and no principal ref; with no
principal ref it had no event kinds, so the governance tap could not observe it
even in principle.

Three validators passed over it. validate_module_descriptors reported "ok (30
descriptors)", check_module_source_ownership reported "ok (15 contracts, 1
legacy root)", check_module_bus_boundary reported "ok (126 declared crossings)".
None of them was wrong. Descriptors are read to find what to BUILD, so a file
nobody declares is never looked at rather than refused: undeclared state was
INVISIBLE rather than ILLEGAL.

This closes that. A companion check owns the inside of `server-go/modules/`
(every non-test .go file there claimed by exactly one descriptor); this one owns
the COMPLEMENT — everything under server-go/ that is not in a module tree — and
requires each package to be either declared by a descriptor or named below with
a reason.

WHY THE ENTRIES ARE EXACT PACKAGE PATHS AND NOT PREFIXES
--------------------------------------------------------
The obvious shape is a prefix exemption: allow `server-go/internal/`, since the
WFE slice legitimately lives there. That shape would have permitted
`server-go/internal/peer/` in silence — it would have failed to catch the very
case it was written for. So entries are exact package directories and a new
package under an already-exempt parent is a failure until someone decides,
deliberately, which it is.

A stale entry is also a failure: an exemption matching no files means the code
moved and the exemption did not, which is how an allowlist rots into a place
where anything can hide.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

NAME = "go-package-ownership"

MODULES_TREE = "server-go/modules"

# Exact package directories under server-go/ that no module descriptor claims,
# each with the reason it is not a module. Four of these categories are
# architectural and one is a debt; they are kept in one table so a reader sees
# the whole complement at once rather than inferring it from what is absent.
#
#   contract   the caller-side half of a module's wire. Deliberately outside
#              modules/ so a peer can exchange the contract over the bus without
#              importing the implementation (see server-go/delegate/client.go).
#   substrate  the bus itself. It cannot be a module because a module is defined
#              by attaching to it.
#   host       the process that module binaries are spawned as. A host is not a
#              module; it is what modules run inside.
#   buildtag   //go:build tools — build-time pins, never linked into a binary.
#   debt       owns state and has no owner. Named, with what it would take to
#              resolve. NOT a blessing.
UNOWNED_PACKAGES: dict[str, tuple[str, str]] = {
    "server-go/bus": (
        "substrate",
        "The event bus client, module runtime and wire. Every module imports it; "
        "it cannot itself be a module.",
    ),
    "server-go/cmd/aimee-module": (
        "host",
        "The multicall binary module processes are spawned as, and the dispatch "
        "table binding each module id to its stages.",
    ),
    "server-go/cmd/aimee-server": (
        "host",
        "The server process entrypoint.",
    ),
    "server-go/config": (
        "contract",
        "Caller-side contract for the config module (ref 2). No storage; every "
        "read is a bounded bus request.",
    ),
    "server-go/db1": (
        "contract",
        "Caller-side mirror of the db1 module's serving wire (db1-fields-v2).",
    ),
    "server-go/db2": (
        "contract",
        "Caller-side mirror of the db2 module's serving wire.",
    ),
    "server-go/delegate": (
        "contract",
        "Shared caller-side contract for delegate execution, so a peer can call "
        "delegates without importing the delegates module.",
    ),
    "server-go/tools": (
        "buildtag",
        "//go:build tools — pins build-time modules compiled as standalone "
        "executables by the container build.",
    ),
    "server-go/internal/api": (
        "debt",
        "WFE vertical slice: the /v1 HTTP surface. Owns no durable state itself "
        "but is the writer's front door. Resolving it means deciding whether the "
        "WFE control plane becomes a module or stays the host's own internals.",
    ),
    "server-go/internal/engine": (
        "debt",
        "WFE vertical slice: the workflow state machine and scheduler. Owns "
        "durable workflow state with no principal ref, so the governance tap "
        "cannot observe it.",
    ),
    "server-go/internal/wfe": (
        "debt",
        "WFE vertical slice: workflow definitions, registry and artifact store.",
    ),
    "server-go/internal/db1": (
        "debt",
        "WFE vertical slice: the engine's view of the db1 module. A second "
        "caller-side mapping alongside server-go/db1, which is itself worth "
        "resolving.",
    ),
    "server-go/internal/db1/db1test": (
        "debt",
        "Test support for the slice above; carried with it.",
    ),
}

# Directories whose contents are fixtures rather than shipped code.
FIXTURE_MARKERS = ("/testdata/",)


def module_declared_go_files(root: Path) -> dict[str, list[str]]:
    """Map every Go path a descriptor declares to the descriptors declaring it."""
    declared: dict[str, list[str]] = {}
    modules_dir = root / "src" / "modules"
    for descriptor in sorted(modules_dir.glob("*/module.yaml")):
        try:
            data = json.loads(descriptor.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise SystemExit(f"{NAME}: error: {descriptor}: unreadable descriptor: {exc}")
        for key in ("go_sources", "go_tests"):
            for path in data.get(key, []):
                declared.setdefault(path, []).append(str(data.get("id", descriptor.parent.name)))
    return declared


def go_files_outside_modules(root: Path) -> tuple[list[str], int]:
    """Non-test .go files under server-go/ that are not in a module tree.

    Returns the files AND how many were skipped as fixtures, because a summary
    that reports only what it covered gets quieter as its exclusions grow. The
    covered count and the total shrink together when a skip rule widens, so
    neither number moves in a way a reader would question -- the figure stays
    honest and the omission does the lying.

    The denominator is for the READER. What actually defends this check is
    structural and was verified by planting a widened skip rule rather than by
    reasoning: every file in scope is either named in UNOWNED_PACKAGES or has no
    home at all, so removing a directory from the walk either strands an
    exemption -- which the stale-entry arm reports -- or drops a file that would
    have errored as undeclared. A skip cannot quietly shrink this set.

    That is worth stating because it is the difference between sound by
    construction and sound by luck, and the summary line alone cannot tell you
    which one you have.
    """
    found: list[str] = []
    skipped = 0
    base = root / "server-go"
    if not base.is_dir():
        raise SystemExit(f"{NAME}: error: {base}: no server-go tree")
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if d != ".git")
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        if rel_dir == MODULES_TREE or rel_dir.startswith(MODULES_TREE + "/"):
            continue
        if any(marker in "/" + rel_dir + "/" for marker in FIXTURE_MARKERS):
            skipped += sum(
                1 for f in filenames if f.endswith(".go") and not f.endswith("_test.go")
            )
            continue
        for filename in sorted(filenames):
            if not filename.endswith(".go") or filename.endswith("_test.go"):
                continue
            found.append(f"{rel_dir}/{filename}")
    return sorted(found), skipped


def run(root: Path) -> str:
    declared = module_declared_go_files(root)
    files, skipped = go_files_outside_modules(root)

    # A check that compared NOTHING must not report ok.
    #
    # Both of these can come back empty without anything raising: the descriptor
    # glob matches no module.yaml, or the walk finds no .go files, because a
    # directory moved. The run then completes, finds no violations, and prints
    # "0 file(s) accounted for" -- a count of zero rendered as an accomplishment.
    #
    # A peer hit this in a validator whose heading pattern stopped matching after
    # an ordinary edit: it took the not-checked-and-no-problems path and returned
    # 0. Their own docstring named that failure for an EMPTY GLOB and guarded it,
    # while the same condition one level in returned success.
    if not declared:
        raise SystemExit(
            f"{NAME}: error: no module.yaml declared any Go file. Either the "
            f"descriptors moved or the glob stopped matching; both mean this "
            f"check compared nothing."
        )
    if not files:
        raise SystemExit(
            f"{NAME}: error: found no non-test .go files outside "
            f"{MODULES_TREE}. This check exists to account for them, so finding "
            f"none means the tree moved, not that everything is owned."
        )

    errors: list[str] = []
    matched_entries: set[str] = set()
    covered = 0

    for path in files:
        package = path.rsplit("/", 1)[0]
        entry = UNOWNED_PACKAGES.get(package)
        owners = declared.get(path, [])

        if entry and owners:
            errors.append(
                f"{path}: claimed by descriptor(s) {sorted(owners)} AND exempt as "
                f"'{entry[0]}'. Exactly one must be true — remove whichever is wrong."
            )
            continue
        if owners:
            if len(owners) > 1:
                errors.append(f"{path}: claimed by more than one descriptor: {sorted(owners)}")
            covered += 1
            continue
        if entry:
            matched_entries.add(package)
            covered += 1
            continue

        errors.append(
            f"{path}: package '{package}' owns code under server-go/ but no module "
            f"descriptor declares it and it is not named in UNOWNED_PACKAGES.\n"
            f"    A package with no descriptor has no id, no dependency edges and no "
            f"principal ref — and with no principal ref it has no event kinds, so the "
            f"governance tap cannot observe it even in principle.\n"
            f"    THE REPAIR is to add the file to go_sources in "
            f"src/modules/<id>/module.yaml, so the code has an owner.\n"
            f"    Adding '{package}' to UNOWNED_PACKAGES is NOT a cheaper version\n"
            f"    of that. It is a CLAIM: that this package should never be a\n"
            f"    module, and so should never be observable. An entry added to\n"
            f"    silence this message records the invisibility as permission for\n"
            f"    it. Use it only if the claim is true, and say why it is true."
        )

    for package, (kind, _reason) in sorted(UNOWNED_PACKAGES.items()):
        if package not in matched_entries:
            errors.append(
                f"{package}: exempt as '{kind}' but no non-test .go file matches it. "
                f"The code moved and the exemption did not; remove or repoint it."
            )

    if errors:
        for error in errors:
            print(f"{NAME}: error: {error}", file=sys.stderr)
        raise SystemExit(1)

    debts = sum(1 for kind, _ in UNOWNED_PACKAGES.values() if kind == "debt")
    return (
        f"{NAME}: ok ({covered} of {len(files)} file(s) outside {MODULES_TREE}/ "
        f"accounted for, {skipped} skipped as fixtures; "
        f"{len(UNOWNED_PACKAGES)} named non-module package(s), {debts} of them debt)"
    )


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    print(run(root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
