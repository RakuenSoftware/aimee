#!/usr/bin/env python3
"""Exercise one full-memory EuroBERT training step before a long sweep."""

from __future__ import annotations

import argparse
import json
import platform
import time
from pathlib import Path
from typing import Any

from train_eurobert_reranker import (
    expected_provenance,
    load_spec,
    sha256,
    verify_loaded_model,
    write_json_atomic,
)


def expected_smoke_provenance(
    manifest_path: Path,
    manifest: dict[str, Any],
    model: dict[str, Any],
) -> dict[str, Any]:
    return {
        **expected_provenance(manifest_path, manifest, model),
        "smoke_script_sha256": sha256(Path(__file__).resolve()),
    }


def assert_completed_smoke(path: Path, expected: dict[str, Any]) -> dict[str, Any]:
    if not path.exists():
        raise RuntimeError(f"runtime smoke record is missing: {path}")
    actual = json.loads(path.read_text(encoding="utf-8"))
    comparable = {key: actual.get(key) for key in expected}
    if comparable != expected:
        raise RuntimeError(f"runtime smoke provenance does not match: {path}")
    if actual.get("status") != "complete":
        raise RuntimeError(f"runtime smoke is not marked complete: {path}")
    return actual


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    manifest, spec = load_spec(args.manifest, args.label)
    training = manifest["training"]
    expected = expected_smoke_provenance(args.manifest, manifest, spec)

    import torch
    from sentence_transformers import CrossEncoder
    from sentence_transformers.cross_encoder.losses import MSELoss
    from transformers import set_seed

    if not torch.cuda.is_available():
        raise RuntimeError("ROCm device is not available through torch.cuda")
    if not torch.cuda.is_bf16_supported():
        raise RuntimeError("the selected ROCm device does not report bf16 support")

    seed = int(training["seed"])
    set_seed(seed)
    model = CrossEncoder(
        spec["repository"],
        revision=spec["revision"],
        device="cuda",
        num_labels=1,
        max_length=int(training["max_length"]),
        activation_fn=torch.nn.Identity(),
        model_kwargs={"attn_implementation": "sdpa"},
        config_kwargs={"classifier_pooling": spec["classifier_pooling"]},
    )
    verify_loaded_model(model, spec)
    model.gradient_checkpointing_enable()
    model.train()

    batch_size = int(training["per_device_batch_size"])
    max_length = int(training["max_length"])
    long_text = "benchmark " * (max_length * 2)
    queries = [f"query {index} {long_text}" for index in range(batch_size)]
    documents = [f"document {index} {long_text}" for index in range(batch_size)]
    pairs = list(zip(queries, documents, strict=True))
    tokenized = model.preprocess(pairs)
    observed_tokens = int(tokenized["input_ids"].shape[1])
    if observed_tokens != max_length:
        raise RuntimeError(f"expected a {max_length}-token smoke batch, received {observed_tokens}")

    labels = torch.linspace(-1.0, 1.0, batch_size, device=model.device)
    loss_fn = MSELoss(model, activation_fn=torch.nn.Identity())
    optimizer = torch.optim.AdamW(
        model.parameters(),
        lr=float(spec.get("learning_rate", training["learning_rate"])),
    )
    torch.cuda.empty_cache()
    torch.cuda.reset_peak_memory_stats()
    started = time.monotonic()
    with torch.autocast(device_type="cuda", dtype=torch.bfloat16):
        loss = loss_fn([queries, documents], labels)
    if not torch.isfinite(loss):
        raise RuntimeError(f"runtime smoke produced a non-finite loss: {float(loss)}")
    loss.backward()
    optimizer.step()
    optimizer.zero_grad(set_to_none=True)
    torch.cuda.synchronize()

    result = {
        **expected,
        "status": "complete",
        "environment": {
            "hostname": platform.node(),
            "python": platform.python_version(),
            "torch": torch.__version__,
            "hip": torch.version.hip,
            "device": torch.cuda.get_device_name(0),
        },
        "measurement": {
            "batch_size": batch_size,
            "tokens_per_pair": observed_tokens,
            "loss": float(loss.detach().cpu()),
            "elapsed_seconds": time.monotonic() - started,
            "peak_allocated_bytes": torch.cuda.max_memory_allocated(),
            "peak_reserved_bytes": torch.cuda.max_memory_reserved(),
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json_atomic(args.output, result)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
