#!/usr/bin/env python3
"""Create the immutable, balanced 32-cell provider matrix plan."""

import argparse
import json
import random
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cell-artifacts", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    runner = root / "benchmarks/code-agent-effectiveness/e6_provider_cell.py"
    arms = ["standard", "observe", "on", "ceiling"]
    tasks = [f"c{number:02d}" for number in range(1, 9)]
    random.Random(20260730).shuffle(tasks)
    cells = []
    for index, task in enumerate(tasks):
        block = arms[:]
        random.Random(20260730 + index).shuffle(block)
        for arm in block:
            cell_id = f"{arm}-{task}"
            cells.append({
                "id": cell_id,
                "command": ["python3", str(runner), "--arm", arm, "--task", task,
                            "--artifacts", str(args.cell_artifacts / cell_id)],
                "timeout_seconds": 960,
            })
    plan = {
        "schema_version": 1,
        "pinned_commit": "aa8c40e9d75449774c9b0b630bb8f1037efb8097",
        "model": "gpt-5.6-sol",
        "reasoning": "medium",
        "prompt_fixture": "prompts/e6-agent-task-v1.md",
        "schedule_seed": 20260730,
        "cells": cells,
    }
    args.output.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
