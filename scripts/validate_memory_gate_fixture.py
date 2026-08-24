#!/usr/bin/env python3
"""Validate the committed temporal/learning golden gate without model calls."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


REQUIRED_CASES = {
    "newer_correction_closes_interval",
    "late_older_evidence_preserves_newer_fact",
    "duplicate_claim_adds_evidence",
    "lower_authority_contradiction_quarantined",
    "valid_and_believed_axes_diverge",
    "current_excludes_inactive_lifecycle",
    "historical_results_are_labeled",
    "temporal_filter_applies_every_hop",
    "out_of_order_ingestion_is_deterministic",
    "erasure_retires_observation",
    "unauthorized_observation_is_withheld",
    "failed_applied_procedure_requires_review",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--learning", action="store_true")
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.fixture.read_text().splitlines() if line.strip()]
    if not rows:
        raise SystemExit("empty fixture cannot pass")
    manifest, *cases = rows
    if manifest.get("record_type") != "manifest" or manifest.get("status") != "complete":
        raise SystemExit("fixture manifest is absent or incomplete")
    ids = {row.get("case_id") for row in cases}
    missing = sorted(REQUIRED_CASES - ids)
    if missing:
        raise SystemExit(f"missing temporal/contradiction cases: {missing}")
    for row in cases:
        if row.get("context_sufficiency") not in {"COMPLETE", "PARTIAL", "INSUFFICIENT"}:
            raise SystemExit(f"invalid sufficiency grade: {row.get('case_id')}")
        if row.get("authority_violations") != 0 or row.get("scope_violations") != 0:
            raise SystemExit(f"authority/scope violation: {row.get('case_id')}")
        if row.get("expected") != row.get("observed"):
            raise SystemExit(f"golden mismatch: {row.get('case_id')}")
        if not isinstance(row.get("channel_metrics"), dict):
            raise SystemExit(f"channel metrics missing: {row.get('case_id')}")
    calibration = manifest.get("judge_calibration", {})
    if calibration.get("human_scored_cases", 0) < 12 or calibration.get("agreement", 0.0) < 0.9:
        raise SystemExit("human calibration contract is below the committed floor")
    if args.learning:
        metrics = manifest.get("learning_metrics", {})
        floors = {
            "recurrence_precision": 0.9,
            "recovery_accuracy": 0.9,
            "correction_adherence": 1.0,
            "attributed_application_rate": 1.0,
        }
        for key, floor in floors.items():
            if metrics.get(key, 0.0) < floor:
                raise SystemExit(f"learning metric below floor: {key}")
        if metrics.get("negative_transfer", 1.0) > 0.0:
            raise SystemExit("negative transfer must be zero in the golden corpus")
    print(f"golden gate: pass ({len(cases)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
