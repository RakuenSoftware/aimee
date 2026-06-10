#!/usr/bin/env python3
"""Replay learning-router fixtures and report detector precision/recall.

Closes the harness gap for the learning-router rollout flags
(`learning_synthesize_enabled`, `learning_implicit_*`): the labelled corpora under
benchmarks/learning/ already exist, but there was no runner to grade a detector
against them. This is that runner.

Two fixture families are graded:

  implicit-signal/labelled.jsonl   — per-turn detector. Each row:
      {"heuristic": str, "user_text": str, "expected": bool, "notes": str}
      "expected" = should the implicit-signal detector fire on this turn.

  substrate/*.jsonl                — promotion decision. Each row:
      {"id": str, "candidate_kind": str, "shape": str, "evidence": [...],
       "expected_promote": bool, "expected_tag": str, ...}
      "expected_promote" = should the learning router promote this candidate.

Predictions are *injected*, not computed here — the harness must not duplicate the C
detector logic (that would validate nothing). Bind it to a live aimee build by passing
`--predictions FILE`, a jsonl emitted by replaying the same fixtures through the real
detector. Each prediction row is `{"predicted": bool}` (optionally `{"id": str}`);
rows match fixtures by "id" when both sides carry one, else positionally in file order.

Without `--predictions`, the harness runs in *validation mode*: it schema-checks the
fixtures and prints the label distribution (a useful CI smoke + readiness check), then
exits 0 without asserting a quality bar.

Exit status in graded mode: 0 if every pinned threshold clears, 1 otherwise — so this
can gate a rollout the way poison_gate.py / guardrails_replay.py do.

DETECTOR-REPLAY ENTRY (citation heuristics): `src/tests/learning_implicit_replay.c`
(built as `make tests/learning-implicit-replay`) runs the real
dogfood_classify_next_turn() over the citation_then_* fixtures and emits the
predictions jsonl. Grade that subset with `--heuristics
citation_then_repair,citation_then_continuation`. The three stateful heuristics
(repeat_question, repeated_correction, workflow_repetition) still need a live
router + session/DB state to replay.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

# Pinned acceptance criteria (see docs/validation/flag-rollout-readiness.md).
# Override on the CLI; defaults are the project's standard bar.
DEFAULT_MIN_PRECISION = 0.90  # of fires, ≥90% should be true positives
DEFAULT_MIN_RECALL = 0.80  # of true signals, ≥80% should fire
DEFAULT_MAX_FALSE_POSITIVE_RATE = 0.10  # of negatives, ≤10% may wrongly fire
MIN_FIXTURES_FOR_STABLE_RATE = 10  # below this, rates are too noisy to gate on


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
    return rows


def _expected_field(row: dict[str, Any]) -> str:
    """Which key carries the ground-truth boolean for this fixture family."""
    if "expected" in row:
        return "expected"
    if "expected_promote" in row:
        return "expected_promote"
    raise ValueError(f"row has no expected/expected_promote field: {sorted(row)}")


def _validate(rows: list[dict[str, Any]], path: Path) -> None:
    for i, row in enumerate(rows):
        field = _expected_field(row)  # raises if absent
        if not isinstance(row[field], bool):
            raise ValueError(f"{path}[{i}]: {field} must be a boolean")


def _align_predictions(
    fixtures: list[dict[str, Any]], preds: list[dict[str, Any]], path: Path
) -> list[bool]:
    """Return predicted booleans aligned to fixtures (by id if available, else order)."""
    fixture_ids = [r.get("id") for r in fixtures]
    pred_ids = [p.get("id") for p in preds]
    if all(fixture_ids) and all(pred_ids):
        by_id = {p["id"]: bool(p["predicted"]) for p in preds}
        missing = [fid for fid in fixture_ids if fid not in by_id]
        if missing:
            raise ValueError(f"{path}: predictions missing ids: {missing[:5]}")
        return [by_id[fid] for fid in fixture_ids]
    if len(preds) != len(fixtures):
        raise ValueError(
            f"{path}: positional match needs equal counts "
            f"(fixtures={len(fixtures)}, predictions={len(preds)})"
        )
    return [bool(p["predicted"]) for p in preds]


def _score(fixtures: list[dict[str, Any]], predicted: list[bool]) -> dict[str, Any]:
    tp = fp = tn = fn = 0
    for row, pred in zip(fixtures, predicted):
        truth = bool(row[_expected_field(row)])
        if truth and pred:
            tp += 1
        elif truth and not pred:
            fn += 1
        elif not truth and pred:
            fp += 1
        else:
            tn += 1
    precision = tp / (tp + fp) if (tp + fp) else 1.0
    recall = tp / (tp + fn) if (tp + fn) else 1.0
    fpr = fp / (fp + tn) if (fp + tn) else 0.0
    return {
        "n": len(fixtures),
        "tp": tp,
        "fp": fp,
        "tn": tn,
        "fn": fn,
        "precision": round(precision, 4),
        "recall": round(recall, 4),
        "false_positive_rate": round(fpr, 4),
    }


def _label_distribution(fixtures: list[dict[str, Any]]) -> dict[str, int]:
    pos = sum(1 for r in fixtures if bool(r[_expected_field(r)]))
    return {"total": len(fixtures), "positive": pos, "negative": len(fixtures) - pos}


def _collect(inputs: list[str]) -> list[Path]:
    paths: list[Path] = []
    for item in inputs:
        p = Path(item)
        if p.is_dir():
            paths.extend(sorted(p.glob("*.jsonl")))
        else:
            paths.append(p)
    if not paths:
        raise ValueError(f"no .jsonl fixtures found under {inputs}")
    return paths


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixtures", nargs="+", help="fixture .jsonl files or dirs")
    ap.add_argument(
        "--heuristics",
        help="comma-separated heuristic names; keep only rows whose 'heuristic' is in this set "
        "(use to grade the subset a partial detector-replay covers, e.g. the citation heuristics)",
    )
    ap.add_argument("--predictions", help="jsonl of {'predicted': bool[, 'id']} from a live detector replay")
    ap.add_argument("--min-precision", type=float, default=DEFAULT_MIN_PRECISION)
    ap.add_argument("--min-recall", type=float, default=DEFAULT_MIN_RECALL)
    ap.add_argument("--max-fpr", type=float, default=DEFAULT_MAX_FALSE_POSITIVE_RATE)
    ap.add_argument("--output", help="write the JSON report here")
    args = ap.parse_args(argv)

    fixture_paths = _collect(args.fixtures)
    fixtures: list[dict[str, Any]] = []
    for path in fixture_paths:
        rows = _load_jsonl(path)
        _validate(rows, path)
        fixtures.extend(rows)

    if args.heuristics:
        keep = {h.strip() for h in args.heuristics.split(",") if h.strip()}
        fixtures = [r for r in fixtures if r.get("heuristic") in keep]
        if not fixtures:
            raise ValueError(f"no fixtures match --heuristics {sorted(keep)}")

    report: dict[str, Any] = {
        "fixtures": [str(p) for p in fixture_paths],
        "label_distribution": _label_distribution(fixtures),
    }

    if not args.predictions:
        report["mode"] = "validation"
        report["note"] = "no --predictions bound; schema + distribution only, no quality gate"
        _emit(report, args.output)
        print("VALIDATION OK (no predictions bound — see --predictions to grade)", file=sys.stderr)
        return 0

    preds = _load_jsonl(Path(args.predictions))
    predicted = _align_predictions(fixtures, preds, Path(args.predictions))
    metrics = _score(fixtures, predicted)
    report["mode"] = "graded"
    report["metrics"] = metrics
    report["thresholds"] = {
        "min_precision": args.min_precision,
        "min_recall": args.min_recall,
        "max_false_positive_rate": args.max_fpr,
    }

    failures = []
    if metrics["n"] < MIN_FIXTURES_FOR_STABLE_RATE:
        failures.append(f"too few fixtures ({metrics['n']} < {MIN_FIXTURES_FOR_STABLE_RATE}) — keep default-off")
    if metrics["precision"] < args.min_precision:
        failures.append(f"precision {metrics['precision']} < {args.min_precision}")
    if metrics["recall"] < args.min_recall:
        failures.append(f"recall {metrics['recall']} < {args.min_recall}")
    if metrics["false_positive_rate"] > args.max_fpr:
        failures.append(f"fpr {metrics['false_positive_rate']} > {args.max_fpr}")

    report["passed"] = not failures
    report["failures"] = failures
    _emit(report, args.output)
    for line in (failures or ["PASS — all thresholds clear"]):
        print(("FAIL: " if failures else "") + line, file=sys.stderr)
    return 1 if failures else 0


def _emit(report: dict[str, Any], output: str | None) -> None:
    text = json.dumps(report, indent=2)
    if output:
        Path(output).write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    raise SystemExit(main())
