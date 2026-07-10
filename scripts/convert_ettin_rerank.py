#!/usr/bin/env python3
"""Convert an ettin cross-encoder reranker to the runtime form aimee-llm serves:
the ENCODER as a GGUF (via llama.cpp's convert_hf_to_gguf.py) plus the Dense score
head as a numpy .npz (keys W2/W4/b4/gamma/beta, matching EttinRerankHead.from_npz).

CI-only helper (see .github/workflows/publish-rerank-artifacts.yml). The ST score
head doesn't survive GGUF conversion, so the gateway applies it from the .npz at
runtime. Hoisted here so neither the build nor the runtime image needs torch.

Usage: convert_ettin_rerank.py <size> <hf_repo> <out_dir> <llama_cpp_dir>
"""
import os
import subprocess
import sys

import numpy as np
from huggingface_hub import snapshot_download
from safetensors.numpy import load_file


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    size, repo, out_dir, llama = sys.argv[1:5]
    dest = f"/tmp/ettin-{size}"
    print(f"snapshot {repo} -> {snapshot_download(repo, local_dir=dest)}")

    gguf = os.path.join(out_dir, f"rerank-ettin-{size}.gguf")
    env = dict(os.environ, PYTHONPATH=os.path.join(llama, "gguf-py"))
    subprocess.run(
        [sys.executable, os.path.join(llama, "convert_hf_to_gguf.py"),
         dest, "--outfile", gguf, "--outtype", "f16"],
        check=True, env=env,
    )

    W2 = load_file(f"{dest}/2_Dense/model.safetensors")["linear.weight"]
    d4 = load_file(f"{dest}/4_Dense/model.safetensors")
    ln = load_file(f"{dest}/3_LayerNorm/model.safetensors")
    npz = os.path.join(out_dir, f"rerank-ettin-{size}.head.npz")
    np.savez(npz, W2=W2, W4=d4["linear.weight"], b4=d4["linear.bias"],
             gamma=ln["norm.weight"], beta=ln["norm.bias"])
    print(f"wrote {gguf} + {npz}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
