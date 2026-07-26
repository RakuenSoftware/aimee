#!/usr/bin/env python3
"""Download the pinned six-model baseline artifacts with resume and verification."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("models.json"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--reuse-26b", type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for model in manifest["models"]:
        target = args.output_dir / model["file"]
        if model["label"] == "gemma4_26b_a4b" and args.reuse_26b and args.reuse_26b.exists() and not target.exists():
            try:
                os.link(args.reuse_26b, target)
            except OSError:
                shutil.copyfile(args.reuse_26b, target)
        url = f"https://huggingface.co/{model['repository']}/resolve/{model['revision']}/{model['file']}"
        if not target.exists() or target.stat().st_size != model["bytes"]:
            subprocess.run(["curl", "--fail", "--location", "--retry", "8", "--retry-all-errors", "--continue-at", "-", "--output", str(target), url], check=True)
        if target.stat().st_size != model["bytes"]:
            raise RuntimeError(f"size mismatch for {target}: {target.stat().st_size} != {model['bytes']}")
        sha256 = digest(target)
        if model.get("sha256") and sha256 != model["sha256"]:
            raise RuntimeError(f"SHA-256 mismatch for {target}")
        results.append({"label": model["label"], "file": model["file"], "bytes": target.stat().st_size, "sha256": sha256})
        print(json.dumps(results[-1]), flush=True)
    (args.output_dir / "ARTIFACTS.json").write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
