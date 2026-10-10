"""Prepare isolated Gemma4 E2B serving config; never start a service or enroll it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets

from bridge import SYSTEM_CONTEXT, read_secret


def write_private(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        stream.write(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True, help="local model/config/tokenizer directory")
    parser.add_argument("--gguf", type=Path, required=True, help="Gemma4 E2B UD-Q4_K_XL weights")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model-key", type=Path, required=True)
    options = parser.parse_args()
    model, gguf = options.model.resolve(strict=True), options.gguf.resolve(strict=True)
    config = json.loads((model / "config.json").read_text())
    text = config.get("text_config", config)
    if (config.get("model_type") != "gemma4" or text.get("hidden_size") != 1536
            or text.get("num_hidden_layers") != 35):
        raise ValueError("model directory does not match the Gemma4 E2B binding")
    if not any((model / name).is_file() for name in ("tokenizer.json", "tokenizer.model")):
        raise ValueError("model directory must contain its matching tokenizer")
    digest = hashlib.sha256()
    with gguf.open("rb") as stream:
        if stream.read(4) != b"GGUF":
            raise ValueError("weights are not a GGUF file")
        stream.seek(0)
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    output = options.output.expanduser().resolve()
    key = options.model_key.expanduser().resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    key.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if (output / "serving.json").exists():
        raise ValueError("serving.json already exists; review it before replacing")
    value = {
        "adapter": "gemma4-e2b", "model": str(model), "gguf_weights": str(gguf),
        "gguf_weights_sha256": digest.hexdigest(), "model_revision": digest.hexdigest(),
        "gguf_weight_profile": "UD-Q4_K_XL", "gguf_matrix_backend": "triton",
        "enable_hybrid_native": True, "host_memory_budget": 1073741824,
        "device_memory_budget": 268435456, "native_system_context": SYSTEM_CONTEXT,
        "serve": {"host": "127.0.0.1", "port": 19852, "tensor_parallel_size": 1,
                  "gpu_memory_utilization": 0.75, "max_model_len": 2048,
                  "max_num_seqs": 1, "max_num_batched_tokens": 128,
                  "enable_prefix_caching": False, "async_scheduling": False,
                  "v2_model_runner": False, "enable_model_compilation": False,
                  "enable_piecewise_graphs": False, "cpu_offload_gb": 0}
    }
    if not key.exists():
        write_private(key, secrets.token_urlsafe(48) + "\n")
    else:
        read_secret(key)
    write_private(output / "serving.json", json.dumps(value, indent=2) + "\n")
    print("E2B serving configuration prepared. Review the checkpoint profile before serving.")


if __name__ == "__main__":
    main()
