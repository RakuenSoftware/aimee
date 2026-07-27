#!/usr/bin/env python3
"""Generate paired Ettin/EuroBERT reranking reports with explicit caveats."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import subprocess
from pathlib import Path
from typing import Any


ETTIN_LABELS = ("ettin68m", "ettin400m")
ETTIN_TRAINING_EXAMPLES = 143_393_475


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def model_context(label: str, eurobert_examples: int) -> dict[str, Any]:
    if label in ETTIN_LABELS:
        return {
            "family": "Ettin",
            "model_state": "released_reranker",
            "scoring_mode": "cross_encoder_scalar_score",
            "reranker_training_examples": ETTIN_TRAINING_EXAMPLES,
        }
    if label.endswith("_pretrained"):
        return {
            "family": "EuroBERT",
            "model_state": "official_pretrained_base",
            "scoring_mode": "mean_pooled_bi_encoder_cosine_similarity",
            "reranker_training_examples": 0,
        }
    return {
        "family": "EuroBERT",
        "model_state": "ettin_teacher_score_finetuned",
        "scoring_mode": "cross_encoder_scalar_score",
        "reranker_training_examples": eurobert_examples,
    }


def comparison_context(left: str, right: str, eurobert_examples: int) -> dict[str, Any]:
    left_model = model_context(left, eurobert_examples)
    right_model = model_context(right, eurobert_examples)
    involves_published_ettin = left in ETTIN_LABELS or right in ETTIN_LABELS
    involves_pretrained_control = left.endswith("_pretrained") or right.endswith("_pretrained")
    if involves_pretrained_control:
        latency_qualification = (
            "not_comparable: stock EuroBERT is a mean-pooled bi-encoder cosine quality control, "
            "not a cross-encoder latency baseline"
        )
    elif involves_published_ettin:
        latency_qualification = (
            "diagnostic_only: published Ettin append-only logs mix the original serialized profile "
            "with the corrected concurrent continuation; quality deltas remain paired"
        )
    else:
        latency_qualification = "qualified: both trained EuroBERT runs use the same clean cross-encoder load profile"
    context: dict[str, Any] = {
        "quality_pairing": "same frozen cases, candidate order, input bounds, and scoring metrics",
        "latency_qualification": latency_qualification,
        "models": {left: left_model, right: right_model},
        "training_budget_equal": (
            left_model["reranker_training_examples"] == right_model["reranker_training_examples"]
        ),
    }
    if involves_published_ettin:
        context.update({
            "ettin_training_examples": ETTIN_TRAINING_EXAMPLES,
            "eurobert_training_examples": (
                0 if involves_pretrained_control else eurobert_examples
            ),
            "training_budget_note": (
                "Released Ettin checkpoints used the full dataset; trained EuroBERT uses the declared bounded "
                "subset, while stock EuroBERT has seen zero reranker examples."
            ),
        })
    return context


def require_restored_state(path: Path) -> dict[str, Any]:
    state = json.loads(path.read_text(encoding="utf-8"))
    if state.get("status") != "complete" or state.get("production_restored") is not True:
        raise RuntimeError(f"benchmark state is not complete with production restored: {path}")
    return state


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--main-state", type=Path, required=True)
    parser.add_argument("--eurobert-state", type=Path, required=True)
    parser.add_argument("--compare-script", type=Path, default=Path(__file__).with_name("compare_ab.py"))
    args = parser.parse_args()

    require_restored_state(args.main_state)
    require_restored_state(args.eurobert_state)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    eurobert_labels = tuple(
        label for model in manifest["models"] for label in (model["pretrained_label"], model["label"])
    )
    eurobert_examples = len(manifest["training"]["configs"]) * int(manifest["training"]["examples_per_config"])
    labels = (*ETTIN_LABELS, *eurobert_labels)
    states = {
        **{label: args.main_state for label in ETTIN_LABELS},
        **{label: args.eurobert_state for label in eurobert_labels},
    }

    output_dir = args.results / "reranker_pairwise"
    output_dir.mkdir(exist_ok=True)
    reports = []
    for left, right in itertools.combinations(labels, 2):
        output = output_dir / f"reranking_{left}_vs_{right}.json"
        command = [
            "python3",
            str(args.compare_script),
            "--kind",
            "reranking",
            "--left",
            str(args.results / left / f"raw_reranking_{left}.jsonl"),
            "--right",
            str(args.results / right / f"raw_reranking_{right}.jsonl"),
            "--left-label",
            left,
            "--right-label",
            right,
            "--left-summary",
            str(args.results / left / f"summary_reranking_{left}.json"),
            "--right-summary",
            str(args.results / right / f"summary_reranking_{right}.json"),
            "--left-environment",
            str(states[left]),
            "--right-environment",
            str(states[right]),
            "--output",
            str(output),
        ]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        report = json.loads(output.read_text(encoding="utf-8"))
        report["comparison_context"] = comparison_context(left, right, eurobert_examples)
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        reports.append({"left": left, "right": right, "file": output.name, "sha256": sha256(output)})

    ettin_summary = json.loads(
        (args.results / ETTIN_LABELS[0] / f"summary_reranking_{ETTIN_LABELS[0]}.json").read_text(encoding="utf-8")
    )
    index = {
        "models": labels,
        "pair_count": len(reports),
        "reports": reports,
        "suite_manifest_sha256": ettin_summary["suite_manifest_sha256"],
    }
    (output_dir / "INDEX.json").write_text(json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(index, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
