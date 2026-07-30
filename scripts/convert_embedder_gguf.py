#!/usr/bin/env python3
"""Convert a sentence-transformers embedder to the GGUF the aimee-llm gateway serves.

Exists because not every embedder worth offering has a GGUF on the Hub. bekko-a25m is
the case in point: it is a ModernBERT encoder, ~6x nomic's CPU throughput on our corpus,
and published only as safetensors. llama.cpp converts ModernBERT encoders fine (the
retired ettin reranker artifacts were produced the same way), so the conversion is
mechanical — it just has to happen somewhere, once, with the result verifiable.

CI-only helper (see .github/workflows/publish-embedder-artifacts.yml). Neither the build
nor the runtime image needs torch: the conversion runs in CI and the runtime fetches the
published artifact, checked against the release's SHA256SUMS.

WHY THE DIGEST IS NOT IN THE REGISTRY. scripts/embedders.json pins a sha256 for every
`source: hf` entry, because that file already exists and cannot change under a pinned
revision. A converted artifact does not exist until this script runs, so there is nothing
to pin ahead of time; the checksum travels with the release instead. That is the whole
reason `source: release` is a separate shape rather than a blank sha256.

This script does NOT decide pooling, prefixes or width — those live in the registry,
derived from the model card. It only produces weights. It does verify that the width it
converted matches what the registry declares, because a mismatch there is the one error
that would otherwise surface as vectors silently written at the wrong size.

Usage: convert_embedder_gguf.py <model-key> <out_dir> <llama_cpp_dir> [--outtype f16]
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REGISTRY = ROOT / "embedders.json"


def _registry_entry(model_key):
    with open(REGISTRY, "r", encoding="utf-8") as handle:
        table = json.load(handle)["embedders"]
    for name, spec in table.items():
        if name.split("@", 1)[0].strip().lower() == model_key.strip().lower():
            return name, spec
    raise SystemExit(f"convert_embedder_gguf: {model_key!r} is not in {REGISTRY}")


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model_key")
    ap.add_argument("out_dir")
    ap.add_argument("llama_cpp_dir")
    ap.add_argument("--outtype", default="f16")
    args = ap.parse_args()

    name, spec = _registry_entry(args.model_key)
    if str(spec.get("source", "hf")) != "release":
        raise SystemExit(
            f"convert_embedder_gguf: {name} is source={spec.get('source', 'hf')!r}; only a "
            f"`release` entry is converted here (an `hf` entry is fetched directly)"
        )
    repo = str(spec.get("hf_repo", "")).strip()
    revision = str(spec.get("hf_revision", "")).strip() or "main"
    out_file = str(spec.get("file", "")).strip()
    if not repo or not out_file:
        raise SystemExit(f"convert_embedder_gguf: {name} needs hf_repo and file")

    from huggingface_hub import snapshot_download

    src = snapshot_download(repo, revision=revision, local_dir=f"/tmp/{args.model_key}")
    print(f"snapshot {repo}@{revision} -> {src}", flush=True)

    os.makedirs(args.out_dir, exist_ok=True)
    gguf = os.path.join(args.out_dir, out_file)
    env = dict(os.environ, PYTHONPATH=os.path.join(args.llama_cpp_dir, "gguf-py"))
    subprocess.run(
        [sys.executable, os.path.join(args.llama_cpp_dir, "convert_hf_to_gguf.py"),
         src, "--outfile", gguf, "--outtype", args.outtype],
        check=True, env=env,
    )

    # Cross-check the converted width against the declaration. The registry's dim is what
    # sizes the pgvector columns and what the gateway enforces per call, so a disagreement
    # here means one of the two is wrong and the artifact must not ship.
    declared = int(spec["dim"])
    pooling_cfg = Path(src) / "1_Pooling" / "config.json"
    if pooling_cfg.exists():
        with open(pooling_cfg, "r", encoding="utf-8") as handle:
            actual = int(json.load(handle).get("word_embedding_dimension") or 0)
        if actual and actual != declared:
            raise SystemExit(
                f"convert_embedder_gguf: {name} declares dim {declared} but {repo} reports "
                f"{actual}; fix the registry entry before publishing an artifact"
            )

    digest = _sha256(gguf)
    sums = os.path.join(args.out_dir, "SHA256SUMS")
    with open(sums, "a", encoding="utf-8") as handle:
        handle.write(f"{digest}  {out_file}\n")
    print(f"wrote {gguf}\n{digest}  {out_file}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
