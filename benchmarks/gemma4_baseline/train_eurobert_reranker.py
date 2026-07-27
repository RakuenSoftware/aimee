#!/usr/bin/env python3
"""Train one pinned EuroBERT cross-encoder on a fixed Ettin-data subset."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def collect_final_artifacts(final_dir: Path) -> list[dict[str, Any]]:
    required = {
        "config.json",
        "config_sentence_transformers.json",
        "modules.json",
        "tokenizer_config.json",
    }
    present = {path.name for path in final_dir.iterdir() if path.is_file()}
    missing = required - present
    if missing:
        raise RuntimeError(f"final model is missing required artifacts: {', '.join(sorted(missing))}")
    if not any(
        name in present
        for name in ("model.safetensors", "model.safetensors.index.json", "pytorch_model.bin")
    ):
        raise RuntimeError("final model has no saved weights or weight index")

    artifacts = []
    for path in sorted(candidate for candidate in final_dir.rglob("*") if candidate.is_file()):
        artifacts.append(
            {
                "path": path.relative_to(final_dir).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256(path),
            }
        )
    return artifacts


def assert_completed_training_dir(output_dir: Path, expected: dict[str, Any]) -> dict[str, Any]:
    provenance_path = output_dir / "training_provenance.json"
    if not provenance_path.exists():
        raise RuntimeError(f"training completion record is missing: {provenance_path}")
    actual = json.loads(provenance_path.read_text(encoding="utf-8"))
    comparable = {key: actual.get(key) for key in expected}
    if comparable != expected:
        raise RuntimeError(f"training completion provenance does not match: {output_dir}")
    if actual.get("status") != "complete":
        raise RuntimeError(f"training is not marked complete: {output_dir}")

    final_dir = output_dir / "final"
    recorded = actual.get("final_artifacts")
    if not isinstance(recorded, list) or not recorded:
        raise RuntimeError(f"final artifact manifest is missing: {output_dir}")
    observed = collect_final_artifacts(final_dir)
    if observed != recorded:
        raise RuntimeError(f"final artifact verification failed: {output_dir}")
    return actual


def load_spec(path: Path, label: str) -> tuple[dict[str, Any], dict[str, Any]]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    matches = [model for model in manifest["models"] if model["label"] == label]
    if len(matches) != 1:
        raise ValueError(f"expected one model labeled {label!r}, found {len(matches)}")
    return manifest, matches[0]


def expected_provenance(manifest_path: Path, manifest: dict[str, Any], model: dict[str, Any]) -> dict[str, Any]:
    training = manifest["training"]
    return {
        "manifest_sha256": sha256(manifest_path),
        "model": model,
        "training": training,
        "training_examples": len(training["configs"]) * int(training["examples_per_config"]),
    }


def assert_compatible_provenance(path: Path, expected: dict[str, Any]) -> None:
    if not path.exists():
        return
    actual = json.loads(path.read_text(encoding="utf-8"))
    comparable = {key: actual.get(key) for key in expected}
    if comparable != expected:
        raise RuntimeError(f"refusing to reuse incompatible training directory: {path.parent}")


def load_training_data(training: dict[str, Any]):
    from datasets import concatenate_datasets, load_dataset

    limit = int(training["examples_per_config"])
    datasets = []
    for config in training["configs"]:
        dataset = load_dataset(
            training["dataset"],
            config,
            split=f"train[:{limit}]",
            revision=training["dataset_revision"],
        )
        if len(dataset) != limit:
            raise RuntimeError(f"{config}: expected {limit} examples, received {len(dataset)}")
        if set(dataset.column_names) != {"query", "document", "label"}:
            raise RuntimeError(f"{config}: unexpected columns {dataset.column_names}")
        datasets.append(dataset)
    return concatenate_datasets(datasets)


def verify_loaded_model(model: Any, spec: dict[str, Any]) -> None:
    config = model.transformers_model.config
    expected = {
        "model_type": spec["expected_model_type"],
        "hidden_size": spec["expected_hidden_size"],
        "num_hidden_layers": spec["expected_layers"],
    }
    actual = {key: getattr(config, key, None) for key in expected}
    if actual != expected:
        raise RuntimeError(f"loaded model identity mismatch: expected {expected}, received {actual}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    manifest, spec = load_spec(args.manifest, args.label)
    training = manifest["training"]
    expected = expected_provenance(args.manifest, manifest, spec)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    provenance_path = args.output_dir / "training_provenance.json"
    assert_compatible_provenance(provenance_path, expected)
    write_json_atomic(provenance_path, {**expected, "status": "training"})

    import torch
    from sentence_transformers import CrossEncoder
    from sentence_transformers.cross_encoder import CrossEncoderTrainer, CrossEncoderTrainingArguments
    from sentence_transformers.cross_encoder.losses import MSELoss
    from transformers import set_seed
    from transformers.trainer_utils import get_last_checkpoint

    if not torch.cuda.is_available():
        raise RuntimeError("ROCm device is not available through torch.cuda")
    if training["precision"] == "bf16" and not torch.cuda.is_bf16_supported():
        raise RuntimeError("the selected ROCm device does not report bf16 support")

    seed = int(training["seed"])
    set_seed(seed)
    train_dataset = load_training_data(training)
    model = CrossEncoder(
        spec["repository"],
        revision=spec["revision"],
        num_labels=1,
        max_length=int(training["max_length"]),
        activation_fn=torch.nn.Identity(),
        model_kwargs={"attn_implementation": "sdpa"},
    )
    verify_loaded_model(model, spec)
    loss = MSELoss(model, activation_fn=torch.nn.Identity())
    checkpoint_dir = args.output_dir / "checkpoints"
    train_args = CrossEncoderTrainingArguments(
        output_dir=str(checkpoint_dir),
        num_train_epochs=float(training["epochs"]),
        per_device_train_batch_size=int(training["per_device_batch_size"]),
        gradient_accumulation_steps=int(training["gradient_accumulation_steps"]),
        learning_rate=float(training["learning_rate"]),
        warmup_ratio=float(training["warmup_ratio"]),
        bf16=training["precision"] == "bf16",
        fp16=training["precision"] == "fp16",
        gradient_checkpointing=bool(training["gradient_checkpointing"]),
        seed=seed,
        data_seed=seed,
        save_strategy="steps",
        save_steps=2000,
        save_total_limit=2,
        logging_steps=100,
        report_to="none",
        dataloader_num_workers=min(8, os.cpu_count() or 1),
    )
    trainer = CrossEncoderTrainer(model=model, args=train_args, train_dataset=train_dataset, loss=loss)
    checkpoint = get_last_checkpoint(str(checkpoint_dir)) if args.resume and checkpoint_dir.exists() else None
    trainer.train(resume_from_checkpoint=checkpoint)
    final_dir = args.output_dir / "final"
    model.save_pretrained(str(final_dir))
    final_artifacts = collect_final_artifacts(final_dir)

    completed = {
        **expected,
        "status": "complete",
        "environment": {
            "hostname": platform.node(),
            "python": platform.python_version(),
            "torch": torch.__version__,
            "hip": torch.version.hip,
            "device": torch.cuda.get_device_name(0),
        },
        "train_metrics": trainer.state.log_history[-1] if trainer.state.log_history else {},
        "final_model": str(final_dir),
        "final_artifacts": final_artifacts,
    }
    write_json_atomic(provenance_path, completed)
    print(json.dumps(completed, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
