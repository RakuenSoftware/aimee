#!/usr/bin/env python3
"""Validate an S1 provider probe from a descendant release checkout.

The frozen S1 probe records the complete ``src`` tree used for the paired
study.  A PR merge checkout can legitimately contain later, unrelated source
files from its base branch.  This validator keeps the evidence pin immutable
while proving that every semantic-context file is byte-identical to the
candidate, that its effective build commands are unchanged, and that the
real-provider observations satisfy the frozen gate.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
CANDIDATE_BASE = "cccb1490560810a3da92691f381f9ab59040bb7c"

# This is the complete tracked src/ surface changed by the semantic-context
# candidate relative to its testing base.  Keep it explicit: widening or
# narrowing the release lineage boundary must be reviewable in the PR diff.
PROTECTED_PATHS = (
    "src/Makefile",
    "src/modules/lsp/lsp.h",
    "src/modules/lsp/lsp_context.c",
    "src/modules/lsp/lsp_context.h",
    "src/modules/lsp/lsp_manager.c",
    "src/modules/protocols/mcp/mcp_tools.c",
    "src/modules/protocols/mcp/mcp_tools_extended.c",
    "src/posix/platform_ipc.c",
    "src/server/server_mcp_call_table.c",
    "src/tests/Rules.mk",
    "src/tests/support/config_module_stub.c",
    "src/tests/test_lsp.c",
    "src/tests/test_mcp_client_registry.c",
)

# The proxy adds a thin-client source and a separate test prerequisite. Neither
# changes the LSP probe's inputs or recipe. Do not exempt entire Makefiles:
# removing an LSP object, changing flags, or weakening a test must still fail.
# Pairs are (reviewed release text, frozen equivalent), not regexes or globs.
REVIEWED_BUILD_INTEGRATIONS = {
    "src/Makefile": (
        ("CLI_SRCS += http_content_encoding.c\nCLI_SRCS += cli_proxy.c\n",
         "CLI_SRCS += http_content_encoding.c\n"),
    ),
    "src/tests/Rules.mk": (
        ('.PHONY: proxy-tests\nproxy-tests: $(BINARY)\n'
         '\tAIMEE_TEST_PROXY_BINARY="$(abspath $(BINARY))" python3 ../scripts/tests/test_thin_client_proxy.py -v\n\n'
         'unit-tests: $(UNIT_TEST_P1_PREREQ) $(BINARY) proxy-tests ',
         'unit-tests: $(UNIT_TEST_P1_PREREQ) $(BINARY) '),
    ),
}


def reviewed_build_equivalent(path: str, frozen: str, current: str) -> bool:
    integrations = REVIEWED_BUILD_INTEGRATIONS.get(path)
    if not integrations:
        return False
    for release_text, frozen_text in integrations:
        if current.count(release_text) == 1:
            current = current.replace(release_text, frozen_text, 1)
    return current.strip() == frozen.strip()


# Shared build files also describe every other subsystem. Their LSP compile and
# link commands are protected, rather than unrelated targets elsewhere in them.
SHARED_BUILD_PATHS = ("src/Makefile", "src/tests/Rules.mk")


def lsp_build_plan(makefile: str, rules: str, scratch: Path) -> str:
    include = "include tests/Rules.mk"
    if makefile.count(include) != 1:
        raise ValueError("cannot locate the unique native test rules include")
    (scratch / "Rules.mk").write_text(rules)
    (scratch / "Makefile").write_text(
        makefile.replace(include, "include " + str(scratch / "Rules.mk"))
    )
    target = str(scratch / "obj/tests/unit-test-lsp")
    result = subprocess.run(
        ["make", "--no-print-directory", "--dry-run", "--always-make",
         "-f", str(scratch / "Makefile"), "OBJDIR=" + str(scratch / "obj"),
         "GIT_VERSION=ci", "GIT_COMMIT_TIME=1700000000", target],
        cwd=ROOT / "src", text=True, capture_output=True, check=True,
    )
    plan = result.stdout
    if "lsp_context.c" not in plan or "-o " + target not in plan:
        raise ValueError("LSP build plan lacks its context compilation or probe link")
    return plan


def build_contract_matches(candidate_commit: str) -> bool:
    with tempfile.TemporaryDirectory(prefix="aimee-lsp-build-contract-") as directory:
        scratch = Path(directory)
        candidate = lsp_build_plan(
            git_output("show", candidate_commit + ":src/Makefile"),
            git_output("show", candidate_commit + ":src/tests/Rules.mk"), scratch,
        )
        release = lsp_build_plan(
            (ROOT / "src/Makefile").read_text(),
            (ROOT / "src/tests/Rules.mk").read_text(), scratch,
        )
        return candidate == release


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe-report", type=Path, required=True)
    parser.add_argument("--candidate-commit", required=True)
    parser.add_argument("--candidate-src-tree", required=True)
    parser.add_argument("--cold-starts", type=int, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--assert-release-candidate", action="store_true")
    return parser.parse_args()


def git_output(*args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=ROOT, text=True, capture_output=True, check=True
    ).stdout.strip()


def provider_errors(report: dict[str, Any], cold_starts: int) -> list[str]:
    errors: list[str] = []
    providers = report.get("providers")
    if not isinstance(providers, list):
        return ["probe report does not contain a provider list"]
    names = [provider.get("name") for provider in providers]
    if names != ["gopls", "pyright"]:
        errors.append("probe report must contain gopls and pyright in order")
    minimum_successes = math.ceil(cold_starts * 0.95)
    for provider in providers:
        name = provider.get("name", "unknown")
        if not provider.get("available"):
            errors.append(f"{name}: pinned binary is missing")
        if provider.get("cold_start_attempts") != cold_starts:
            errors.append(f"{name}: cold-start denominator is incomplete")
        if provider.get("cold_start_successes", 0) < minimum_successes:
            errors.append(f"{name}: fewer than 95 percent of clean cold starts passed")
        trials = provider.get("trials")
        if not isinstance(trials, list) or len(trials) != cold_starts:
            errors.append(f"{name}: retained trial denominator is incomplete")
        if provider.get("reference_recall") != 1.0:
            errors.append(f"{name}: checked reference recall is incomplete")
        if provider.get("reference_false_positive_rate") != 0.0:
            errors.append(f"{name}: checked reference result has false positives")
    return errors


def validate(
    report: dict[str, Any], candidate_commit: str, candidate_src_tree: str,
    cold_starts: int,
) -> dict[str, Any]:
    errors: list[str] = []
    head = git_output("rev-parse", "HEAD")
    release_src_tree = git_output("rev-parse", "HEAD:src")
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", candidate_commit, "HEAD"], cwd=ROOT
    ).returncode == 0
    candidate_paths = git_output(
        "diff", "--name-only", CANDIDATE_BASE, candidate_commit, "--", "src"
    ).splitlines()
    changed = git_output(
        "diff", "--name-only", candidate_commit, "--", *PROTECTED_PATHS
    ).splitlines()
    reviewed_build_paths = []
    for path in changed:
        if path in REVIEWED_BUILD_INTEGRATIONS and reviewed_build_equivalent(
            path, git_output("show", f"{candidate_commit}:{path}"),
            (ROOT / path).read_text(),
        ):
            reviewed_build_paths.append(path)
    changed = [path for path in changed if path not in reviewed_build_paths]
    untracked = git_output(
        "ls-files", "--others", "--exclude-standard", "--", *PROTECTED_PATHS
    ).splitlines()

    if not ancestor:
        errors.append("frozen candidate commit is not an ancestor of the release checkout")
    if candidate_paths != list(PROTECTED_PATHS):
        errors.append("protected path manifest does not equal the candidate source diff")
    changed_sources = [path for path in changed if path not in SHARED_BUILD_PATHS]
    build_matches = True
    if any(path in SHARED_BUILD_PATHS for path in changed):
        try:
            build_matches = build_contract_matches(candidate_commit)
        except (OSError, ValueError, subprocess.CalledProcessError) as exc:
            build_matches = False
            errors.append("could not verify LSP build contract: " + str(exc))
    if changed_sources:
        errors.append("protected semantic-context files differ from the frozen candidate")
    if not build_matches:
        errors.append("LSP compile or link commands differ from the frozen candidate")
    if untracked:
        errors.append("untracked files overlap the protected semantic-context surface")
    if report.get("schema_version") != 1:
        errors.append("probe report schema version is not 1")
    if report.get("candidate_commit") != candidate_commit:
        errors.append("probe report candidate commit does not match the release pin")
    if report.get("candidate_src_tree") != candidate_src_tree:
        errors.append("probe report source tree does not match the frozen evidence pin")
    if report.get("checkout_src_tree") != release_src_tree:
        errors.append("probe report was not produced from the current release source tree")
    if report.get("cold_starts_per_provider") != cold_starts:
        errors.append("probe report cold-start denominator does not match the release gate")
    errors.extend(provider_errors(report, cold_starts))

    return {
        "schema_version": 1,
        "purpose": "S1 descendant release-checkout validation",
        "candidate_commit": candidate_commit,
        "candidate_src_tree": candidate_src_tree,
        "candidate_base_commit": CANDIDATE_BASE,
        "candidate_changed_src_paths": candidate_paths,
        "release_commit": head,
        "release_src_tree": release_src_tree,
        "probe_source_commit": report.get("source_commit"),
        "probe_checkout_src_tree": report.get("checkout_src_tree"),
        "candidate_is_ancestor": ancestor,
        "protected_paths": list(PROTECTED_PATHS),
        "changed_protected_paths": changed,
        "build_contract_matched": build_matches,
        "reviewed_build_integration_paths": reviewed_build_paths,
        "untracked_protected_paths": untracked,
        "cold_starts_per_provider": cold_starts,
        "release_candidate_matched": not errors,
        "release_candidate_errors": errors,
    }


def main() -> int:
    args = parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.candidate_commit):
        raise SystemExit("--candidate-commit must be a full lowercase commit SHA")
    if not re.fullmatch(r"[0-9a-f]{40}", args.candidate_src_tree):
        raise SystemExit("--candidate-src-tree must be a full lowercase Git tree ID")
    if args.cold_starts < 1:
        raise SystemExit("--cold-starts must be positive")
    report = json.loads(args.probe_report.read_text())
    result = validate(
        report, args.candidate_commit, args.candidate_src_tree, args.cold_starts
    )
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        sys.stdout.write(rendered)
    return 1 if args.assert_release_candidate and result["release_candidate_errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
