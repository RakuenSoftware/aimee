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
    parser.add_argument("--candidate-src-tree", required=True)
    parser.add_argument("--cold-starts", type=int, default=1)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--assert-candidate", action="store_true")
    return parser.parse_args()


def trial_success(provider: dict) -> bool:
    observation = provider.get("observation", {})
    probe = observation.get("probe") or {}
    return bool(
        provider.get("available")
        and not observation.get("timed_out")
        and observation.get("exit_code") == 0
        and probe.get("synchronized")
        and probe.get("document_version") == 1
        and isinstance(probe.get("provider_generation"), (int, float))
        and probe.get("provider_generation", 0) > 0
        and probe.get("definition_matched")
        and probe.get("reference_count") == 3
        and probe.get("active_servers_after_query") == 1
    )


def aggregate_provider(name: str, trials: list[dict]) -> dict:
    successful = sum(trial_success(trial) for trial in trials)
    reference_counts = [
        (trial.get("observation", {}).get("probe") or {}).get("reference_count")
        for trial in trials
        if isinstance((trial.get("observation", {}).get("probe") or {}).get(
            "reference_count"
        ), int)
    ]
    return {
        "name": name,
        "available": all(trial.get("available") for trial in trials),
        "cold_start_attempts": len(trials),
        "cold_start_successes": successful,
        "cold_start_success_rate": successful / len(trials),
        "reference_recall": (
            sum(min(count, 3) / 3 for count in reference_counts) / len(reference_counts)
            if reference_counts else None
        ),
        "reference_false_positive_rate": (
            sum(max(count - 3, 0) / max(count, 1) for count in reference_counts)
            / len(reference_counts) if reference_counts else None
        ),
        "peak_process_tree_count": max(
            (trial.get("observation") or {}).get("peak_process_tree_count", 0)
            for trial in trials
        ),
        "peak_process_tree_rss_kib": max(
            (trial.get("observation") or {}).get("peak_process_tree_rss_kib", 0)
            for trial in trials
        ),
        "trials": trials,
    }


def candidate_errors(report: dict) -> list[str]:
    errors: list[str] = []
    if not report["runtime_tree_matches_candidate"]:
        errors.append("runtime source tree does not match the pinned candidate commit")
    for provider in report["providers"]:
        name = provider["name"]
        if not provider["available"]:
            errors.append(f"{name}: pinned binary is missing")
            continue
        if provider.get("cold_start_attempts") != report["cold_starts_per_provider"]:
            errors.append(f"{name}: cold-start denominator is incomplete")
        if provider.get("cold_start_success_rate", 0) < 0.95:
            errors.append(f"{name}: fewer than 95 percent of clean cold starts passed")
        if provider.get("reference_recall") != 1.0:
            errors.append(f"{name}: checked reference recall is incomplete")
        if provider.get("reference_false_positive_rate") != 0.0:
            errors.append(f"{name}: checked reference result has false positives")
    return errors


def main() -> int:
    args = parse_args()
    if args.cold_starts < 1:
        raise SystemExit("--cold-starts must be positive")
    if not re.fullmatch(r"[0-9a-f]{40}", args.candidate_commit):
        raise SystemExit("--candidate-commit must be a full lowercase commit SHA")
    if not re.fullmatch(r"[0-9a-f]{40}", args.candidate_src_tree):
        raise SystemExit("--candidate-src-tree must be a full lowercase Git tree ID")
    checkout_src_tree = subprocess.run(
        ["git", "rev-parse", "HEAD:src"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    runtime_diff = subprocess.run(
        ["git", "diff", "--quiet", "HEAD", "--", "src"], cwd=ROOT
    )
    untracked_runtime = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "--", "src"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    runtime_tree_matches_candidate = (
        checkout_src_tree == args.candidate_src_tree
        and runtime_diff.returncode == 0
        and not untracked_runtime
    )
    env = os.environ.copy()
    common = {
        "lsp_test": args.lsp_test.resolve(),
        "env": env,
        "probe_mode": "--real-provider-synced",
    }
    provider_specs = [
        (
            "gopls", args.gopls, "-", FIXTURES / "go", "main.go", 8, 9, 3,
            [str(args.gopls), "version"],
        ),
        (
            "pyright", args.pyright_langserver, "--stdio", FIXTURES / "python",
            "sample.py", 6, 12, 1, [str(args.pyright), "--version"],
        ),
    ]
    providers = []
    for name, command, server_arg, fixture, file_name, line, column, expected_line, version in provider_specs:
        trials = [
            provider_record(
                name, command, server_arg, fixture, file_name, line, column, expected_line, 3,
                version_command=version, **common,
            )
            for _ in range(args.cold_starts)
        ]
        providers.append(aggregate_provider(name, trials))
    report = {
        "schema_version": 1,
        "purpose": "S1 candidate saved-file synchronization correctness probe",
        "candidate_commit": args.candidate_commit,
        "candidate_src_tree": args.candidate_src_tree,
        "checkout_src_tree": checkout_src_tree,
        "runtime_tree_matches_candidate": runtime_tree_matches_candidate,
        "source_commit": subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, capture_output=True, check=True
        ).stdout.strip(),
        "environment": {"platform": platform.platform(), "python": platform.python_version()},
        "cold_starts_per_provider": args.cold_starts,
        "providers": providers,
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
