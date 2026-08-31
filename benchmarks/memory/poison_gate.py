#!/usr/bin/env python3
"""Deterministic clean-vs-poison gate for outcome-driven memory demotion.

The fixture intentionally gives poisoned rows high declared confidence,
trusted-looking source tags, and high retrieval frequency. The replay scorer
must ignore those fields and suppress rows only from closed retrieval outcomes.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

NEGATIVE_VERDICTS = {"corrected", "contradicted", "rolled_back"}
POSITIVE_VERDICTS = {"accepted"}


def tokens(text: str) -> set[str]:
    return {tok for tok in re.findall(r"[a-z0-9_]+", text.lower()) if len(tok) > 2}


def demotion_score(row: dict[str, Any], n_min: int) -> float | None:
    attributions = row.get("attributions") or []
    if len(attributions) < n_min:
        return None
    score = 0.0
    for item in attributions:
        verdict = str(item.get("verdict", "irrelevant"))
        weight = float(item.get("weight", 1.0))
        if verdict in POSITIVE_VERDICTS:
            score += weight
        elif verdict in NEGATIVE_VERDICTS:
            score -= weight
    return score


def row_is_suppressed(row: dict[str, Any], n_min: int) -> bool:
    score = demotion_score(row, n_min)
    return score is not None and score < 0.0


def retrieve(query: str, rows: list[dict[str, Any]], n_min: int) -> str | None:
    query_tokens = tokens(query)
    best_id = None
    best_score = -1
    for row in rows:
        if row_is_suppressed(row, n_min):
            continue
        overlap = len(query_tokens & tokens(str(row.get("content", ""))))
        if overlap > best_score:
            best_id = str(row["id"])
            best_score = overlap
    return best_id


def evaluate_scenario(scenario: dict[str, Any], n_min: int) -> dict[str, Any]:
    questions = scenario["questions"]
    rows = scenario["rows"]
    results = []
    correct = 0
    for question in questions:
        retrieved_id = retrieve(question["query"], rows, n_min)
        ok = retrieved_id == question["expected_row_id"]
        correct += 1 if ok else 0
        results.append(
            {
                "question_id": question["id"],
                "expected_row_id": question["expected_row_id"],
                "retrieved_row_id": retrieved_id,
                "verdict": "CORRECT" if ok else "WRONG",
            }
        )
    suppressed = sorted(row["id"] for row in rows if row_is_suppressed(row, n_min))
    return {
        "name": scenario["name"],
        "accuracy": correct / len(questions) if questions else 0.0,
        "correct": correct,
        "total": len(questions),
        "suppressed_row_ids": suppressed,
        "results": results,
    }


def validate_forbidden_metadata_ignored(payload: dict[str, Any]) -> list[str]:
    errors = []
    for scenario in payload["scenarios"]:
        for row in scenario["rows"]:
            poisoned_metadata = (
                float(row.get("declared_confidence", 0.0)) >= 0.95
                and str(row.get("source_tag", "")).startswith("operator")
                and int(row.get("retrieval_frequency", 0)) >= 50
            )
            if poisoned_metadata and not row_is_suppressed(row, int(payload["n_min"])):
                errors.append(
                    f"{scenario['name']}:{row['id']} has trusted-looking metadata but no negative "
                    "outcome window; fixture would not test metadata-spoof resilience"
                )
    return errors


def fixture_label(fixture_path: Path) -> str:
    """Repo-relative path for the report, falling back to absolute.

    The report is committed, so an absolute path made it churn on every run from
    a different checkout -- two runs of an identical, fully deterministic gate
    produced a diff whose only content was somebody's worktree name. That is a
    spurious diff in review and it hides real ones.
    """
    resolved = fixture_path.resolve()
    repo_root = Path(__file__).resolve().parents[2]
    try:
        return resolved.relative_to(repo_root).as_posix()
    except ValueError:
        # Fixture supplied from outside the repo via --fixtures; absolute is the
        # only meaningful label, and such a run is not the committed baseline.
        return str(resolved)


def run(fixture_path: Path) -> tuple[int, dict[str, Any]]:
    payload = json.loads(fixture_path.read_text())
    n_min = int(payload["n_min"])
    delta_max = float(payload["delta_max"])
    scenarios = {str(item["name"]): evaluate_scenario(item, n_min) for item in payload["scenarios"]}
    clean = scenarios.get("clean")
    poison = scenarios.get("poison")
    if clean is None or poison is None:
        raise ValueError("fixture must contain clean and poison scenarios")
    delta = clean["accuracy"] - poison["accuracy"]
    metadata_errors = validate_forbidden_metadata_ignored(payload)
    report = {
        "version": 1,
        "benchmark": "memory_poison_resilience",
        "fixture": fixture_label(fixture_path),
        "delta_max": delta_max,
        "accuracy_clean": clean["accuracy"],
        "accuracy_poison": poison["accuracy"],
        "delta_clean_poison": delta,
        "gate": "pass" if delta <= delta_max and not metadata_errors else "fail",
        "metadata_boundary": {
            "forbidden_inputs": [
                "source_tag",
                "declared_confidence",
                "author_id",
                "retrieval_frequency",
            ],
            "errors": metadata_errors,
        },
        "scenarios": [clean, poison],
    }
    return (0 if report["gate"] == "pass" else 1), report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fixtures",
        type=Path,
        default=Path(__file__).with_name("poison_fixtures.json"),
        help="Path to poison fixture JSON.",
    )
    parser.add_argument("--output", type=Path, help="Optional JSON report path.")
    args = parser.parse_args()

    rc, report = run(args.fixtures)
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n")
    print(encoded)
    return rc


if __name__ == "__main__":
    sys.exit(main())
