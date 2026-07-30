#!/usr/bin/env python3
"""Combine a complete provider matrix with the pinned E6 retrieval record."""

import argparse
import json
from pathlib import Path


ARMS = ("standard", "observe", "on", "ceiling")
TASKS = tuple(f"c{number:02d}" for number in range(1, 9))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cells", type=Path, required=True)
    parser.add_argument("--retrieval-record", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-retrieval-reuse", action="store_true")
    parser.add_argument("--reuse-rationale")
    args = parser.parse_args()
    rows = []
    for path in sorted(args.cells.glob("*/cell-result.json")):
        row = json.loads(path.read_text())
        rows.append(row)
    keys = [(row.get("arm"), row.get("task")) for row in rows]
    expected = {(arm, task) for arm in ARMS for task in TASKS}
    if len(rows) != 32 or len(set(keys)) != 32 or set(keys) != expected:
        raise ValueError("provider matrix must contain every unique 8-task x 4-arm cell")
    if any(row.get("score_eligible") is not True for row in rows):
        raise ValueError("provider matrix contains an ineligible cell")
    retrieval = json.loads(args.retrieval_record.read_text())
    pinned_commits = {row.get("pinned_commit") for row in rows}
    prompt_fixtures = {row.get("prompt_fixture") for row in rows}
    if len(pinned_commits) != 1 or None in pinned_commits:
        raise ValueError("provider cells must agree on a pinned commit")
    if len(prompt_fixtures) != 1 or None in prompt_fixtures:
        raise ValueError("provider cells must agree on a prompt fixture")
    pinned_commit = pinned_commits.pop()
    prompt_fixture = prompt_fixtures.pop()
    if retrieval.get("prompt_fixture") != prompt_fixture:
        raise ValueError("retrieval record and provider cells use different prompt fixtures")
    retrieval_pin = retrieval["pinned_commit"]
    reuse = None
    if retrieval_pin != pinned_commit:
        if not args.allow_retrieval_reuse or not args.reuse_rationale:
            raise ValueError("commit-mismatched retrieval evidence requires explicit reuse rationale")
        reuse = {"allowed": True, "rationale": args.reuse_rationale}
    result = {
        "schema_version": 1,
        "pinned_commit": pinned_commit,
        "retrieval_pinned_commit": retrieval_pin,
        "retrieval_reuse": reuse,
        "prompt_fixture": prompt_fixture,
        "python_edge_precision": retrieval["python_edge_precision"],
        "python_edge_recall": retrieval["python_edge_recall"],
        "retrieval_cells": retrieval["retrieval_cells"],
        "coding_cells": rows,
    }
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
