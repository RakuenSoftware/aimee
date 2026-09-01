#!/usr/bin/env python3
"""Exercise the S1 saved-file synchronization path with pinned real providers."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_s0_baseline import FIXTURES, ROOT, provider_record  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lsp-test", type=Path, required=True)
    parser.add_argument("--gopls", type=Path, required=True)
    parser.add_argument("--pyright-langserver", type=Path, required=True)
    parser.add_argument("--pyright", type=Path, required=True)
    parser.add_argument("--candidate-commit", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--assert-candidate", action="store_true")
    return parser.parse_args()


def candidate_errors(report: dict) -> list[str]:
    errors: list[str] = []
    if not report["runtime_tree_matches_candidate"]:
        errors.append("runtime source tree does not match the pinned candidate commit")
    for provider in report["providers"]:
        name = provider["name"]
        if not provider["available"]:
            errors.append(f"{name}: pinned binary is missing")
            continue
        observation = provider.get("observation", {})
        probe = observation.get("probe") or {}
        if observation.get("timed_out"):
            errors.append(f"{name}: synchronized probe timed out")
        if observation.get("exit_code") != 0:
            errors.append(f"{name}: synchronized probe exited nonzero")
        if not probe.get("synchronized"):
            errors.append(f"{name}: didOpen synchronization was not acknowledged")
        if probe.get("document_version") != 1:
            errors.append(f"{name}: first synchronized document version was not 1")
        if not isinstance(probe.get("provider_generation"), (int, float)) or probe.get(
            "provider_generation", 0
        ) <= 0:
            errors.append(f"{name}: provider generation was not recorded")
        if not probe.get("definition_matched"):
            errors.append(f"{name}: checked definition target did not resolve")
        if probe.get("reference_count", 0) < 3:
            errors.append(f"{name}: checked references were incomplete")
        if probe.get("active_servers_after_query") != 1:
            errors.append(f"{name}: expected exactly one retained provider")
    return errors


def main() -> int:
    args = parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.candidate_commit):
        raise SystemExit("--candidate-commit must be a full lowercase commit SHA")
    subprocess.run(
        ["git", "cat-file", "-e", f"{args.candidate_commit}^{{commit}}"], cwd=ROOT, check=True
    )
    subprocess.run(
        ["git", "merge-base", "--is-ancestor", args.candidate_commit, "HEAD"],
        cwd=ROOT,
        check=True,
    )
    runtime_diff = subprocess.run(
        ["git", "diff", "--quiet", args.candidate_commit, "--", "src"], cwd=ROOT
    )
    untracked_runtime = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "--", "src"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    runtime_tree_matches_candidate = runtime_diff.returncode == 0 and not untracked_runtime
    env = os.environ.copy()
    common = {
        "lsp_test": args.lsp_test.resolve(),
        "env": env,
        "probe_mode": "--real-provider-synced",
    }
    report = {
        "schema_version": 1,
        "purpose": "S1 candidate saved-file synchronization correctness probe",
        "candidate_commit": args.candidate_commit,
        "runtime_tree_matches_candidate": runtime_tree_matches_candidate,
        "source_commit": subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, capture_output=True, check=True
        ).stdout.strip(),
        "environment": {"platform": platform.platform(), "python": platform.python_version()},
        "providers": [
            provider_record(
                "gopls", args.gopls, "-", FIXTURES / "go", "main.go", 8, 9, 3, 3,
                version_command=[str(args.gopls), "version"], **common,
            ),
            provider_record(
                "pyright", args.pyright_langserver, "--stdio", FIXTURES / "python",
                "sample.py", 6, 12, 1, 3,
                version_command=[str(args.pyright), "--version"], **common,
            ),
        ],
    }
    errors = candidate_errors(report)
    report["candidate_matched"] = not errors
    report["candidate_errors"] = errors
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    else:
        sys.stdout.write(rendered)
    return 1 if args.assert_candidate and errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
