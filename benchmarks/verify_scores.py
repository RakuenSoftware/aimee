#!/usr/bin/env python3
"""Stdlib-only verifier for benchmark harness v2 result files."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from benchmarks.common.harness import category_accuracy
from benchmarks.common.result_schema import (
    label_field_for_dataset,
    require_complete_run,
    run_coverage,
    validate_direct_report,
    validate_direct_result,
    validate_llm_result,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("result_file", nargs="+")
    parser.add_argument(
        "--require-complete",
        action="store_true",
        help=(
            "fail unless every result file proves it came from an uncapped run. "
            "Use whenever a score is promoted rather than merely read: baseline "
            "eligibility, cross-run comparison, published claims."
        ),
    )
    return parser


def verify_file(path: Path) -> dict[str, object]:
    payload = json.loads(path.read_text())
    dataset_name = payload["dataset"]
    label_field = label_field_for_dataset(dataset_name)
    track = payload["track"]
    system_name = str(payload.get("system") or (payload.get("results") or [{}])[0].get("system", "unknown"))
    print(f"{path}: dataset={dataset_name} track={track}")

    # Coverage is printed for every file, complete or not, so the reader never
    # has to infer the question count a score was measured over.
    coverage = run_coverage(payload)
    if coverage is None:
        print("  coverage: unrecorded (cannot be shown to be a complete run)")
    elif coverage["complete"]:
        print(
            f"  coverage: complete "
            f"(samples={coverage['counts']['samples_run']} "
            f"questions={coverage['counts']['questions_run']})"
        )
    else:
        print(
            f"  coverage: PARTIAL "
            f"(max_samples={coverage['limits']['max_samples']} "
            f"max_questions={coverage['limits']['max_questions']}; "
            f"ran samples={coverage['counts']['samples_run']} "
            f"questions={coverage['counts']['questions_run']})"
        )

    if track == "direct" and "segments" in payload and "results" not in payload:
        validate_direct_report(payload, label_field)
        overall_retrieval = next(
            (
                section
                for section in payload["overall"]["sections"]
                if "Retrieval Evaluation" in section["title"]
            ),
            None,
        )
        if overall_retrieval and "latency" in overall_retrieval["metrics"]:
            latency = overall_retrieval["metrics"]["latency"]
            print(
                f"  overall: MRR={overall_retrieval['metrics'].get('mrr', 0.0):.3f} "
                f"Recall@5={overall_retrieval['metrics'].get('recall_5', 0.0):.3f} "
                f"Latency p50={latency.get('p50_ms', 0.0):.3f}ms "
                f"p95={latency.get('p95_ms', 0.0):.3f}ms"
            )
        for segment in payload["segments"]:
            retrieval = next(
                (
                    section
                    for section in segment["report"]["sections"]
                    if "Retrieval Evaluation" in section["title"]
                ),
                None,
            )
            if not retrieval:
                print(
                    f"  {segment['label']}: questions={segment['question_count']} "
                    f"status={segment['report'].get('status', 'unavailable')}"
                )
                continue
            metrics = retrieval["metrics"] if retrieval else {}
            latency = metrics.get("latency", {})
            print(
                f"  {segment['label']}: questions={segment['question_count']} "
                f"MRR={metrics.get('mrr', 0.0):.3f} "
                f"Recall@5={metrics.get('recall_5', 0.0):.3f} "
                f"Latency p50={latency.get('p50_ms', 0.0):.3f}ms "
                f"p95={latency.get('p95_ms', 0.0):.3f}ms"
            )
        return {
            "path": path,
            "dataset": dataset_name,
            "track": track,
            "system": system_name,
            "label_field": label_field,
            "payload": payload,
            "breakdown": {},
        }

    results = payload["results"]
    validator = validate_direct_result if track == "direct" else validate_llm_result
    for row in results:
        validator(row, label_field)
    breakdown = category_accuracy(results, label_field)
    for label, bucket in breakdown.items():
        print(
            f"  {label}: accuracy={bucket['accuracy']:.3f} "
            f"({int(bucket['correct'])}/{int(bucket['total'])})"
        )
    # Print derived LongMemEval metrics if present
    summary = payload.get("summary", {})
    derived = summary.get("derived")
    if derived:
        print(
            f"  derived: factoid_recall={float(derived.get('factoid_recall', 0.0)):.3f} "
            f"abstention_precision={float(derived.get('abstention_precision', 0.0)):.3f} "
            f"false_abstention_rate={float(derived.get('false_abstention_rate', 0.0)):.3f}"
        )
    return {
        "path": path,
        "dataset": dataset_name,
        "track": track,
        "system": system_name,
        "label_field": label_field,
        "payload": payload,
        "breakdown": breakdown,
    }


def _sorted_labels(label_field: str, reports: list[dict[str, object]]) -> list[str]:
    labels = {label for report in reports for label in report["breakdown"].keys()}
    if label_field == "category":
        return sorted(labels, key=lambda item: int(item))
    return sorted(labels)


def render_comparative_group(reports: list[dict[str, object]]) -> str:
    if len(reports) < 2:
        return ""
    dataset = reports[0]["dataset"]
    track = reports[0]["track"]
    label_field = reports[0]["label_field"]

    # Refuse deltas across incompatible judge_profile values (provenance check).
    # Only enforced when at least two reports carry explicit judge_profile fields.
    judge_profiles = {
        str(report["payload"].get("judge_profile"))  # type: ignore[index]
        for report in reports
        if report["payload"].get("judge_profile") is not None  # type: ignore[index]
    }
    if len(judge_profiles) > 1:
        profiles_str = ", ".join(sorted(judge_profiles))
        return (
            f"ERROR: cannot compare across incompatible judge_profile values: {profiles_str}\n"
            "Re-run both benchmarks with the same judge_profile before computing a delta."
        )

    # Refuse deltas across incompatible dataset_hash values.
    dataset_hashes = {
        str(report["payload"].get("dataset_hash"))  # type: ignore[index]
        for report in reports
        if report["payload"].get("dataset_hash") is not None  # type: ignore[index]
    }
    if len(dataset_hashes) > 1:
        return (
            "ERROR: cannot compare across incompatible dataset_hash values.\n"
            "Re-run both benchmarks against the same dataset snapshot before computing a delta."
        )

    lines = [f"Comparative report: dataset={dataset} track={track}"]
    for report in reports:
        summary = report["payload"].get("summary", {})
        lines.append(
            f"  {report['system']}: overall_accuracy={float(summary.get('overall_accuracy', 0.0)):.3f}"
        )
    for label in _sorted_labels(str(label_field), reports):
        parts = []
        for report in reports:
            bucket = report["breakdown"].get(label)
            if not bucket:
                parts.append(f"{report['system']}=n/a")
                continue
            parts.append(
                f"{report['system']}={bucket['accuracy']:.3f} "
                f"({int(bucket['correct'])}/{int(bucket['total'])})"
            )
        lines.append(f"  {label}: " + " ".join(parts))
    return "\n".join(lines)


def main() -> int:
    args = build_parser().parse_args()
    reports = []
    for item in args.result_file:
        reports.append(verify_file(Path(item)))
    if args.require_complete:
        failures = []
        for report in reports:
            try:
                require_complete_run(
                    report["payload"], "score verification", source=str(report["path"])
                )
            except ValueError as exc:
                failures.append(str(exc))
        if failures:
            for message in failures:
                print(f"ERROR {message}", file=sys.stderr)
            return 1
    grouped: dict[tuple[str, str], list[dict[str, object]]] = {}
    for report in reports:
        key = (str(report["dataset"]), str(report["track"]))
        grouped.setdefault(key, []).append(report)
    for key in sorted(grouped):
        rendered = render_comparative_group(grouped[key])
        if rendered:
            print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
