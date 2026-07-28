#!/usr/bin/env python3
"""Generate paired Gemma 4/EuroBERT embedding reports with explicit caveats."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import subprocess
from pathlib import Path
from typing import Any


GEMMA_LABELS = ("gemma4_e2b", "gemma4_e4b")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_completed_state(path: Path, *, production_was_in_scope: bool) -> dict[str, Any]:
    state = json.loads(path.read_text(encoding="utf-8"))
    if state.get("status") != "complete":
        raise RuntimeError(f"benchmark state is not complete: {path}")
    if production_was_in_scope and state.get("production_restored") is not True:
        raise RuntimeError(f"benchmark state does not prove production restoration: {path}")
    if not production_was_in_scope and state.get("production_impacted") is not False:
        raise RuntimeError(f"isolated benchmark state does not prove production was untouched: {path}")
    return state


def model_context(label: str, eurobert_examples: int) -> dict[str, Any]:
    if label in GEMMA_LABELS:
        return {
            "family": "Gemma 4",
            "model_state": "accepted_embedding_checkpoint",
            "dual_role_candidate": False,
        }
    pretrained = label.endswith("_pretrained_encoder")
    return {
        "family": "EuroBERT",
        "model_state": "official_pretrained_base" if pretrained else "ettin_teacher_score_finetuned",
        "pooling": "attention_masked_mean_last_hidden_state_then_l2_normalize",
        "reranker_training_examples": 0 if pretrained else eurobert_examples,
        "dual_role_candidate": not pretrained,
    }


def comparison_context(left: str, right: str, eurobert_examples: int) -> dict[str, Any]:
    eurobert_only = left not in GEMMA_LABELS and right not in GEMMA_LABELS
    return {
        "quality_pairing": "same frozen 10k queries, candidates, positives, and retrieval metrics",
        "latency_qualification": (
            "qualified: both EuroBERT encoders ran on the same RTX 5080 native-CUDA profile"
            if eurobert_only
            else "not_comparable: Gemma 4 ran on the .254 7900 XTX profile and EuroBERT on the .253 5080"
        ),
        "models": {
            left: model_context(left, eurobert_examples),
            right: model_context(right, eurobert_examples),
        },
        "decision_use": (
            "Post-reranker EuroBERT quality determines whether one shared base can replace separate "
            "embedding and reranking artifacts."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--main-state", type=Path, required=True)
    parser.add_argument("--eurobert-state", type=Path, required=True)
    parser.add_argument("--compare-script", type=Path, default=Path(__file__).with_name("compare_ab.py"))
    args = parser.parse_args()

    require_completed_state(args.main_state, production_was_in_scope=True)
    require_completed_state(args.eurobert_state, production_was_in_scope=False)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    eurobert_labels = tuple(
        label
        for model in manifest["models"]
        for label in (model["pretrained_encoder_label"], model["encoder_label"])
    )
    eurobert_examples = len(manifest["training"]["configs"]) * int(
        manifest["training"]["examples_per_config"]
    )
    labels = (*GEMMA_LABELS, *eurobert_labels)
    states = {
        **{label: args.main_state for label in GEMMA_LABELS},
        **{label: args.eurobert_state for label in eurobert_labels},
    }

    output_dir = args.results / "encoder_pairwise"
    output_dir.mkdir(exist_ok=True)
    reports = []
    for left, right in itertools.combinations(labels, 2):
        output = output_dir / f"embedding_{left}_vs_{right}.json"
        command = [
            "python3",
            str(args.compare_script),
            "--kind",
            "embedding",
            "--left",
            str(args.results / left / f"raw_embedding_{left}.jsonl"),
            "--right",
            str(args.results / right / f"raw_embedding_{right}.jsonl"),
            "--left-label",
            left,
            "--right-label",
            right,
            "--left-summary",
            str(args.results / left / f"summary_embedding_{left}.json"),
            "--right-summary",
            str(args.results / right / f"summary_embedding_{right}.json"),
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

    gemma_summary = json.loads(
        (args.results / GEMMA_LABELS[0] / f"summary_embedding_{GEMMA_LABELS[0]}.json").read_text(
            encoding="utf-8"
        )
    )
    index = {
        "models": labels,
        "pair_count": len(reports),
        "reports": reports,
        "suite_manifest_sha256": gemma_summary["suite_manifest_sha256"],
    }
    (output_dir / "INDEX.json").write_text(
        json.dumps(index, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(index, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
