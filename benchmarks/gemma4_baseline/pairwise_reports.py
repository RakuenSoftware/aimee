#!/usr/bin/env python3
"""Generate every six-model paired A:B report from a completed `.254` sweep."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import subprocess
import sys
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--compare-script", type=Path, default=Path(__file__).with_name("compare_ab.py"))
    args = parser.parse_args()
    state_path = args.results / "RUN_STATE.json"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    if state.get("status") != "complete" or state.get("production_restored") is not True:
        raise RuntimeError("sweep is not complete with production restored")
    labels = [model["label"] for model in json.loads(args.models.read_text(encoding="utf-8"))["models"]]
    output_dir = args.results / "pairwise"
    output_dir.mkdir(exist_ok=True)
    reports = []
    for left, right in itertools.combinations(labels, 2):
        for kind in ("synthesis", "embedding"):
            left_dir, right_dir = args.results / left, args.results / right
            if kind == "synthesis":
                left_raw, right_raw = left_dir / f"raw_{left}.jsonl", right_dir / f"raw_{right}.jsonl"
                left_summary, right_summary = left_dir / f"summary_{left}.json", right_dir / f"summary_{right}.json"
            else:
                left_raw = left_dir / f"raw_embedding_{left}.jsonl"
                right_raw = right_dir / f"raw_embedding_{right}.jsonl"
                left_summary = left_dir / f"summary_embedding_{left}.json"
                right_summary = right_dir / f"summary_embedding_{right}.json"
            output = output_dir / f"{kind}_{left}_vs_{right}.json"
            command = [
                "python3", str(args.compare_script), "--kind", kind,
                "--left", str(left_raw), "--right", str(right_raw),
                "--left-label", left, "--right-label", right,
                "--left-summary", str(left_summary), "--right-summary", str(right_summary),
                "--left-environment", str(state_path), "--right-environment", str(state_path),
                "--output", str(output),
            ]
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
            reports.append({"kind": kind, "left": left, "right": right, "file": output.name, "sha256": sha256(output)})
    index = {
        "models": labels,
        "pair_count_per_view": len(list(itertools.combinations(labels, 2))),
        "report_count": len(reports),
        "reports": reports,
    }
    (output_dir / "INDEX.json").write_text(json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(index, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
