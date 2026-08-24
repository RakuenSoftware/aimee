#!/usr/bin/env python3
"""Every *-check target must be reachable from something that runs it.

A check target defined and reached by nothing is the same defect as a test in no
run list: it exists, it is reviewed, it is cited, and it never executes, so it
cannot fail. src/tests/Rules.mk already has a guard for that shape
(check_tests_are_run.py); the Makefile's own gates had none.

REACHABILITY IS A GRAPH, NOT A LIST, and that is the whole reason to compute it
rather than eyeball it. A target may run because:

  - it is named in LINT_CHECKS, which `make lint` runs and CI's lint job invokes
  - it is a PREREQUISITE of something that is reachable, however deep
  - a workflow names it directly
  - a Dockerfile builds it, so it gates at image-publish time

Hand-reading finds the first and misses the second. The session that first
counted these read thirteen orphans; following prerequisite chains reduced it to
five, because four were reached through a chain and one through a Dockerfile.
That is a 60% false-positive rate from treating a graph as a list.

Reports what nothing reaches. Whether such a target SHOULD be wired in is a
question for whoever owns the surface it checks -- adding it to LINT_CHECKS
blind could turn a red gate on for a reason nobody has read.
"""

import glob
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MAKEFILE = REPO_ROOT / "src" / "Makefile"

# `target: prereq prereq` — not a variable assignment, not a pattern rule.
RULE = re.compile(r"^([A-Za-z0-9._/-]+)\s*:(?!=)([^\n=]*)$", re.M)


def load() -> tuple[dict[str, list[str]], str]:
    text = MAKEFILE.read_text()
    joined = re.sub(r"\\\n", " ", text)
    prereqs: dict[str, list[str]] = defaultdict(list)
    for target, deps in RULE.findall(joined):
        if target in {".PHONY", ".DEFAULT_GOAL", ".NOTPARALLEL", ".SUFFIXES"}:
            continue
        prereqs[target].extend(d for d in deps.split() if not d.startswith("$"))
    # Variables expand into prerequisites; LINT_CHECKS is the one that matters.
    for name, value in re.findall(r"^([A-Z_]+)\s*[:+]?=(.*)$", joined, re.M):
        if name == "LINT_CHECKS":
            prereqs["lint"].extend(value.split())
    return prereqs, joined


def reachable_from(roots: list[str], prereqs: dict[str, list[str]]) -> set[str]:
    seen: set[str] = set()
    stack = list(roots)
    while stack:
        node = stack.pop()
        if node in seen:
            continue
        seen.add(node)
        stack.extend(prereqs.get(node, []))
    return seen


def named_elsewhere() -> set[str]:
    """Targets a workflow or Dockerfile builds directly."""
    named: set[str] = set()
    patterns = [str(REPO_ROOT / ".github/workflows/*.yml"), str(REPO_ROOT / "Dockerfile*")]
    for pattern in patterns:
        for path in glob.glob(pattern):
            try:
                body = Path(path).read_text()
            except (OSError, UnicodeDecodeError):
                continue
            named.update(re.findall(r"\b([a-z0-9-]+-(?:check|core))\b", body))
    return named


def documented_manual() -> set[str]:
    """Targets the documentation tells a human to run.

    A gate nothing runs is a defect. A TOOL nothing runs automatically is not:
    `make repository-lock-check` is documented as "when you want it", and
    virtual-context-eval-check is a step in a written validation procedure.
    Reporting those as dead would train a reader to ignore this check's output,
    which costs more than the two lines it saves.

    Documentation is the declaration, exactly as a HISTORICAL marker is for a
    validation record: a target is either wired into something that runs it, or
    a document says a person runs it, and the one state that is not allowed is
    neither.
    """
    named: set[str] = set()
    # CONTRIBUTING.md and friends sit at the root, not under docs/. Scanning
    # only docs/ reported refactor-baseline-check as dead when CONTRIBUTING.md
    # tells a contributor to run it -- a false positive produced by looking in
    # one of the two places documentation lives.
    sources = glob.glob(str(REPO_ROOT / "docs" / "**" / "*.md"), recursive=True)
    sources += glob.glob(str(REPO_ROOT / "*.md"))
    for path in sources:
        try:
            body = Path(path).read_text()
        except (OSError, UnicodeDecodeError):
            continue
        # `make foo-check` and `make -C src foo-check` alike.
        named.update(re.findall(r"make\s+(?:-C\s+\S+\s+)?([a-z0-9-]+-check)\b", body))
    return named


# Check targets that nothing reaches TODAY, each needing a decision by whoever
# owns the surface it guards. Recorded rather than wired in blind: adding one to
# LINT_CHECKS turns on a gate whose failure mode nobody has read, and adding one
# that then fails is how a whole lint target gets disabled.
#
#   cmd-srcs-compile-check, config-schema-drift-check, config-uninitialized-check
#     -- config surfaces. Prerequisites of nothing but .PHONY.
#   token-authority-isolation-check, token-authority-regression-check
#     -- both gate token-authority-core, which NOTHING builds: no Dockerfile, no
#        workflow, not `all`. Its sibling cores are each built by an image. Note
#        Dockerfile.authority-bootstrap explicitly asserts the token-authority
#        binary is ABSENT from that image, so "nothing builds it" may be exactly
#        as intended and these two may be guarding something not yet wired,
#        rather than something that regressed.
#   coverage-check
#     -- enforces coverage floors on high-risk files, including
#        server/server_auth.c at 90%. Depends on `coverage`; named by no
#        workflow, no document, and not in LINT_CHECKS.
#
# Shrinking this list is the fix. An entry that becomes reachable must be
# removed, which the check below enforces so the list cannot rot into a place
# things hide.
UNREACHED = {
    "cmd-srcs-compile-check",
    "config-schema-drift-check",
    "config-uninitialized-check",
    "coverage-check",
    "token-authority-isolation-check",
    "token-authority-regression-check",
}


def main() -> int:
    prereqs, joined = load()
    checks = {t for t in prereqs if t.endswith("-check")}
    if not checks:
        print("check-lint-targets-reachable: no *-check targets found; the Makefile moved")
        return 2

    # `lint` is the PR gate; `all` is the build. Both are entry points, as is
    # anything a workflow or Dockerfile names.
    external = named_elsewhere()
    manual = documented_manual()
    roots = ["lint", "all"] + sorted(external & set(prereqs))
    live = reachable_from(roots, prereqs)

    unreached = {
        c for c in checks
        if c not in live and c not in external and c not in manual
    }

    new = sorted(unreached - UNREACHED)
    if new:
        for name in new:
            print(f"    {name}")
        print(f"\n{len(new)} *-check target(s) are reached by nothing:")
        print("  not in LINT_CHECKS, not a prerequisite of anything reachable, and")
        print("  named by no workflow or Dockerfile. A gate nothing runs cannot fail.")
        print("  Wire it in, document it as a manual tool, or record it in UNREACHED")
        print("  with the reason it is not wired.")
        return 1

    resolved = sorted(UNREACHED - unreached)
    if resolved:
        print(f"UNREACHED names target(s) that are now reached: {', '.join(resolved)}.")
        print("  Remove them, so the list keeps meaning what it says.")
        return 1
    print(f"check-lint-targets-reachable: ok ({len(checks)} check targets; "
          f"{len(manual & checks)} documented as manual)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
