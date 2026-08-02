"""Capture everything needed to reproduce or defend these numbers.

Written because the results are going into a public writeup. A benchmark table
without the exact model revisions and the software stack behind it is not
reproducible, and "we ran Qwen3.5-0.8B" is not a claim anyone can check — model
repos get updated in place.

Emits results/PROVENANCE.json.
"""

import json
import os
import pathlib
import platform
import subprocess

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              timeout=60).stdout.strip() or None
    except Exception:
        return None


def model_revisions():
    from huggingface_hub import HfApi
    api = HfApi()
    reg = json.loads((HERE / "models.json").read_text())
    out = {}
    for m in reg["models"]:
        try:
            i = api.model_info(m["id"])
            cd = i.card_data or {}
            lic = cd.get("license")
            if lic == "other":
                lic = cd.get("license_name", "other")
            out[m["id"]] = {
                "revision": i.sha,
                "licence": lic,
                "last_modified": str(i.last_modified),
                "params_declared": m["params"],
                "class": m["class"],
            }
        except Exception as e:
            out[m["id"]] = {"error": type(e).__name__}
    return out


def main():
    prov = {
        "captured_utc": sh("date -u +%Y-%m-%dT%H:%M:%SZ"),
        "purpose": "Tier-A curator extraction benchmark; see bench/tier-a/REPORT.md",
        "task": {
            "prompt_source": "src/kb/kb_memory_facts.c MF_SYSTEM_PROMPT_TMPL",
            "ontology_source": "src/rel_types.c SEED_ONTOLOGY",
            "parser_mirrors": "mf_commit_facts()",
            "conf_floor": 0.6,
            "decoding": "greedy (do_sample=False), no repetition penalty",
            "max_new_tokens": 512,
            "gold_notes": 70,
            "gold_triples": 64,
            "empty_gold_notes": 23,
        },
        "repo": {
            "branch": sh("git rev-parse --abbrev-ref HEAD"),
            "commit": sh("git rev-parse HEAD"),
            "dirty": bool(sh("git status --porcelain bench/tier-a")),
        },
        "hosts": {
            "gpu_ct": {
                "role": "accuracy runs",
                "note": "LXC 140 on Proxmox host .253, Optane pool, RTX 5080 passthrough",
            },
            "cpu": {
                "role": "llama.cpp speed runs",
                "note": "same LXC, 8 pinned cores of an i7-14700K, Q8_0, "
                        "shared physical host with other CTs — treat as +/-20%",
            },
        },
        "runner": {
            "platform": platform.platform(),
            "python": platform.python_version(),
        },
    }

    try:
        import torch
        import transformers
        prov["runner"].update({
            "torch": torch.__version__,
            "transformers": transformers.__version__,
            "cuda_available": torch.cuda.is_available(),
            "gpu": torch.cuda.get_device_name(0) if torch.cuda.is_available() else None,
            "gpu_capability": list(torch.cuda.get_device_capability(0))
                              if torch.cuda.is_available() else None,
        })
    except Exception as e:
        prov["runner"]["torch_error"] = type(e).__name__

    prov["llama_cpp_commit"] = sh("git -C /opt/llama.cpp rev-parse HEAD")
    prov["models"] = model_revisions()

    out = ROOT / "results" / "PROVENANCE.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(prov, indent=2) + "\n")
    print(f"wrote {out}")
    ok = sum(1 for v in prov["models"].values() if "revision" in v)
    print(f"model revisions captured: {ok}/{len(prov['models'])}")


if __name__ == "__main__":
    main()
