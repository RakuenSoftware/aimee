#!/usr/bin/env python3
"""Every unit-test target must be run by something, or it is not a test.

A target that is defined but named in no run list still looks like coverage.
It appears in the tree, it is reviewed, it is cited in commit messages -- and
it never executes, so it cannot fail. Nine of them were found this way, all of
them bus fixtures: the only proofs that a module process actually SERVES what
its clients ask for. They still compiled and still passed when finally run, so
nothing was broken by them; the cost was that a DB1 module which could not open
its database at all went unnoticed through four cutovers, because the one class
of test that would have caught it was the class nobody ran.

The allowlist below is for targets that genuinely cannot run in the ordinary
suite -- they need Postgres, or a live service. Each entry says which, because
"it needs infrastructure" is a claim that should be checkable rather than a
place to put anything inconvenient.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
RULES = Path("src/tests/Rules.mk")

# Targets the ordinary suite cannot run, and the reason. A -pg target needs a
# live Postgres; a -live target needs a running service. Both are covered by
# their own CI jobs, which is why they are exempt here rather than missing.
INFRASTRUCTURE = {
    "unit-test-content-scope-pg": "needs Postgres",
    "unit-test-kb-audit-worm-pg": "needs Postgres",
    "unit-test-vault-pg": "needs Postgres",
    "unit-test-witness-canary-pg": "needs Postgres",
    "unit-test-witness-checkpoint-produce-pg": "needs Postgres",
    "unit-test-witness-emit-pg": "needs Postgres",
    "unit-test-witness-recovery-pg": "needs Postgres",
    "unit-test-witness-tamper-pg": "needs Postgres",
    "unit-test-kb-bedrock-live": "needs a live Bedrock endpoint",
    "unit-test-kb-mgmt-live": "needs a live management service",
    "unit-test-kb-p2b-egress-live": "needs a live egress path",
    "unit-test-server-management-listener-live": "needs a live listener",
    "unit-test-server-ready": "drives a server process directly",
    "unit-test-memory-audit-hook": "driven by the memory-audit job",
    "unit-test-panel-provider": "driven by the panel-provider job",
}

# Built only under a sanitizer configuration, where the suite adds them itself.
SANITIZE = re.compile(r"^unit-test-db2-[a-z-]+-support-sanitize$")


def fail(message: str) -> int:
    print(f"check_tests_are_run: error: {message}", file=sys.stderr)
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    text = (args.root / RULES).read_text(encoding="utf-8")
    joined = re.sub(r"\\\n", " ", text)

    defined = set(re.findall(r"^\$\(TESTPREFIX\)/(unit-test-[a-z0-9-]+)\s*:", joined, re.M))
    if not defined:
        return fail(f"no test targets found in {RULES}; the pattern stopped matching")

    # TEST_TARGETS is what the suite builds and runs. BUS_TEST_TARGETS is NOT a
    # run list -- it exists to order an archive dependency -- which is exactly
    # the confusion that let these hide: they were named in a variable, so they
    # looked listed.
    run: set[str] = set()
    for match in re.finditer(r"^TEST_TARGETS\s*[:+]?=(.*)$", joined, re.M):
        run.update(re.findall(r"unit-test-[a-z0-9-]+", match.group(1)))

    unrun = sorted(
        name for name in defined - run
        if name not in INFRASTRUCTURE and not SANITIZE.fullmatch(name)
    )
    if unrun:
        listing = "\n".join(f"    {name}" for name in unrun)
        return fail(
            f"{len(unrun)} test target(s) are defined but never run:\n{listing}\n"
            "  Add them to TEST_TARGETS, or to the allowlist in this script with the\n"
            "  infrastructure they need. A target in no run list is not coverage: it\n"
            "  cannot fail, so it cannot tell you anything."
        )

    stale = sorted(name for name in INFRASTRUCTURE if name not in defined)
    if stale:
        return fail(
            f"allowlisted target(s) no longer exist: {', '.join(stale)}. "
            "Remove them, so the list keeps meaning what it says."
        )

    exempt = len(INFRASTRUCTURE) + sum(1 for n in defined if SANITIZE.fullmatch(n))
    print(f"check_tests_are_run: ok ({len(run)} run, {exempt} exempt of {len(defined)} defined)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
