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

# Test sources that no target in Rules.mk builds.
#
# Found while chasing a stale allowlist entry: it claimed two bus-plugin tests
# were "run by `make plugin-e2e`", and neither the targets nor that make target
# existed. Removing the entry fixed the checker and would have buried the real
# finding, which is that the SOURCES are still there and nothing compiles them.
#
# Recorded rather than deleted, and rather than silently exempted, because which
# of these is retired and which is an accident is a question for whoever owns
# each subsystem -- and inventing that answer to make a check pass is how a gap
# becomes a blessing. Each is dead coverage until someone decides: it cannot
# fail, so it cannot tell anyone anything.
#
# Shrinking this list is the fix. An entry that stops being orphaned must be
# removed, which the check below enforces so the list cannot rot into a place
# things hide.
UNBUILT_SOURCES = {
    "test_audit_action.c",
    "test_audit_action_log.c",
    "test_audit_ledger.c",
    "test_bus_arena_tsan.c",
    "test_bus_plugin_process.c",
    "test_bus_plugin_scale.c",
    "test_git_oauth_device.c",
    "test_git_oauth_gh.c",
    "test_git_oauth_github.c",
    "test_git_org_repos.c",
    "test_identity_authority_facade_pg.c",
    "test_kb_mgmt_status_peer.c",
    "test_kb_mgmt_token_authority_client_commit.c",
    "test_kb_mgmt_token_authority_daemon_stop.c",
    "test_panel_ir_contract.c",
    "test_plugin_grant_provisioning.c",
    "test_self_update.c",
}

# Targets the ordinary suite cannot run, and the reason. A -pg target needs a
# live Postgres; a -live target needs a running service. Both are covered by
# their own CI jobs, which is why they are exempt here rather than missing.
INFRASTRUCTURE = {
    # Not a test: the emitter half of unit-test-bus-guardrail-durability, which
    # the shell half runs. It cannot check its own work -- it stops the bus, and
    # the store is reached over that bus -- so the verification is SQL, run
    # after this exits. Listed here rather than added to a run list, because
    # running it alone would emit events and assert nothing about the store.
    "unit-test-bus-guardrail-durability-emit":
        "the emitter half; unit-test-bus-guardrail-durability runs it and verifies in SQL",
    "unit-test-bus-db2-process": "needs Postgres and the packaged DB2 executable",
    "unit-test-content-scope-pg": "needs Postgres",
    "unit-test-pgvec-generation-pg": "needs Postgres and pgvector",
    "unit-test-kb-audit-worm-pg": "needs Postgres",
    "unit-test-vault-pg": "needs Postgres",
    "unit-test-witness-canary-pg": "needs Postgres",
    "unit-test-witness-checkpoint-produce-pg": "needs Postgres",
    "unit-test-witness-emit-pg": "needs Postgres",
    "unit-test-witness-recovery-pg": "needs Postgres",
    "unit-test-witness-tamper-pg": "needs Postgres",

    # Test binaries that are not named unit-test-*. They became visible when
    # the pattern above stopped assuming that prefix, and each is a HARNESS: a
    # program another script drives rather than a test that runs itself. The
    # script that drives it is named, so "who runs this" has an answer that can
    # be checked instead of assumed.
    "bus-conformance-host": "the C half; test_bus_conformance.sh, run by make go-unit-tests",
    "bus-bench": "a measurement; scripts/check_bus_perf_gate.sh drives it",
    "db2-test-template": "builds the Postgres test template; unit-tests-pg drives it",
    "aimee-witness-boot-tpm-harness": "needs swtpm; scripts/run-p7-witness-boot-tpm.sh drives it",
    "aimee-witness-cadence-harness": "needs a live daemon; scripts/run-p7-witness-*.sh drive it",
    "p7-tpm2-harness": "needs swtpm; scripts/p7_tpm2*_test.sh drive it",
    "p7-pkcs11-harness": "needs SoftHSM; scripts/p7_pkcs11_softhsm_test.sh drives it",
    "p7-reseal-d2b-live": "needs swtpm and Postgres; scripts/p7_reseal_d2b_swtpm_pg_test.sh drives it",
    "virtual-context-inspect": "an inspection tool; `make virtual-context-inspect` runs it",

    # Driven by NOTHING. Recorded as such rather than described as covered,
    # because a target nobody runs cannot fail and therefore cannot tell you
    # anything -- and reading it as coverage is worse than knowing it is absent.
    "compaction-retention-probe": "UNRUN: a measurement with no driver; `make compaction-retention-probe`",
    "learning-implicit-replay": "UNRUN: no driver in the tree",
    "mock-mcp-server": "UNRUN: a fixture other tests exec; nothing runs it alone",
    "p7-vault-rewrap-live": "UNRUN: needs swtpm and a live vault; no driver in the tree",
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

    # Targets are declared in TWO shapes, and matching only one hid four.
    #
    #   $(TESTPREFIX)/unit-test-foo:  ...   the binary
    #   unit-test-foo: $(TESTPREFIX)/...    the convenience name people type
    #
    # This originally matched only the first, so a target declared ONLY in the
    # bare shape was invisible to the check written to find unrun targets. Four
    # were hiding there -- and all four were bus fixtures, the same category
    # this script's own history says was the expensive miss. A blind spot in a
    # guard is worth more than the thing it guards, because it makes the tree
    # look checked.
    # And it matched only names beginning "unit-test-", which was the SAME blind
    # spot one layer out. A test binary is a test binary whatever it is called,
    # and the ones that are not unit-test-* are disproportionately the expensive
    # kind. One such harness, the C bus host for an external vector database
    # proof, went unrun -- and broken -- for as long as it existed, because this
    # check could not see the name. That is the third time the same shape has
    # cost something here, so the pattern now takes any target under TESTPREFIX.
    prefixed = set(re.findall(r"^\$\(TESTPREFIX\)/([a-z0-9][a-z0-9-]*)\s*:", joined, re.M))
    bare = set(re.findall(r"^(unit-test-[a-z0-9-]+)\s*:", joined, re.M))
    defined = prefixed | bare
    if not defined:
        return fail(f"no test targets found in {RULES}; the pattern stopped matching")

    # A bare target whose recipe is `$<` and which declares no prerequisite runs
    # NOTHING and reports success -- strictly worse than not being run at all,
    # because adding it to a run list would then pass trivially. Three were in
    # that state; every other bare target names its binary as a prerequisite.
    empty = sorted(
        name for name, prereq in re.findall(r"^(unit-test-[a-z0-9-]+)\s*:([^\n=]*)$", joined, re.M)
        if not prereq.strip()
    )
    if empty:
        listing = "\n".join(f"    {name}" for name in empty)
        return fail(
            f"{len(empty)} test target(s) declare no prerequisite, so `$<` is empty "
            f"and the recipe does nothing:\n{listing}\n"
            "  Give each its binary: `unit-test-foo: $(TESTPREFIX)/unit-test-foo`.\n"
            "  A target that runs nothing and succeeds is worse than one nobody runs."
        )

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

    # The same defect one layer down: a test SOURCE that no target builds.
    #
    # Everything above checks target -> run list. A file with no target at all
    # never reaches that check, so it is invisible to it while looking exactly
    # like coverage: it sits in src/tests/, it is read in review, it is cited,
    # and nothing compiles it. That is a weaker position than an unrun target,
    # which at least still builds.
    sources = sorted((args.root / "src/tests").glob("test_*.c"))
    orphans = sorted(
        path.name for path in sources
        if path.stem not in joined and path.name not in joined
    )
    unexpected = [name for name in orphans if name not in UNBUILT_SOURCES]
    if unexpected:
        listing = "\n".join(f"    src/tests/{name}" for name in unexpected)
        return fail(
            f"{len(unexpected)} test source(s) that no target builds:\n{listing}\n"
            "  Give each a target, delete it, or record it in UNBUILT_SOURCES with\n"
            "  the reason. A source nothing compiles cannot fail, so it is not a test."
        )

    # And the stale-entry rule the allowlist above already has: an entry naming a
    # file that is now built, or gone, is recording something that stopped being
    # true.
    resolved = sorted(name for name in UNBUILT_SOURCES if name not in orphans)
    if resolved:
        return fail(
            f"UNBUILT_SOURCES names test(s) that are no longer orphaned: "
            f"{', '.join(resolved)}. Remove them, so the list keeps meaning what it says."
        )

    exempt = len(INFRASTRUCTURE) + sum(1 for n in defined if SANITIZE.fullmatch(n))
    print(
        f"check_tests_are_run: ok ({len(run)} run, {exempt} exempt of {len(defined)} "
        f"defined; {len(orphans)} source(s) unbuilt)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
