"""Fetch exactly the files the torch path loads for ONE embedder, into HF_HOME.

A FILE RATHER THAN A DOCKERFILE HEREDOC, on purpose. The kb's version of this lived
inside a `RUN ... <<'PYBAKE'`, which cost a build failure the first time someone put an
`if` around it (Docker ends the RUN at the heredoc terminator, so the trailing `fi`
parsed as an instruction: "unknown instruction: fi") and could not be tested without
building an image. Here it is importable and its retry logic has a unit test.

FETCH ONLY WHAT THE LOADER READS. snapshot_download takes the whole repo by default,
and a repo may publish the same weights several times over: bekko ships an onnx/ tree
(nine variants, ~2.2GB) and an openvino/ tree beside its safetensors, none of which
sentence-transformers touches. Excluding the alternate runtimes and the legacy .bin
duplicates keeps this to the safetensors and tokenizer the loader actually opens.
"""
import json
import os
import sys
import time

from huggingface_hub import snapshot_download

REGISTRY = os.environ.get("EMBEDDERS_FILE", "/opt/aimee/embedders.json")

SKIP = [
    "onnx/*", "openvino/*", "*.onnx", "*.onnx_data",      # alternate runtimes
    "*.bin", "*.h5", "*.msgpack", "*.tflite", "*.ckpt",   # duplicate/legacy formats
    "*.gguf",                                             # not this runtime either
]

# Statuses worth another attempt. A bad revision or a repo that does not exist is NOT
# on this list: waiting sixty seconds to repeat a permanent error just moves the failure
# further from its cause.
TRANSIENT = ("429", "500", "502", "503", "504", "Too Many Requests",
             "ReadTimeout", "ConnectionError", "IncompleteRead")


def fetch(repo, **kw):
    """snapshot_download with backoff, because the Hub rate-limits.

    Several image variants build in parallel and each wants the same files, so a bare
    call turns any 429 into a failed image build -- which happened three times in one
    evening, on the publish lane and twice on e2e.
    """
    delay = 15
    for attempt in range(1, 6):
        try:
            return snapshot_download(repo, **kw)
        except Exception as exc:  # noqa: BLE001 - the Hub raises several types here
            text = f"{type(exc).__name__}: {exc}"
            if not any(s in text for s in TRANSIENT) or attempt == 5:
                raise
            print(f"  hub fetch of {repo} failed ({text[:120]}); "
                  f"retry {attempt}/4 in {delay}s", flush=True)
            time.sleep(delay)
            delay *= 2
    raise AssertionError("unreachable")


def code_repos(snapshot_dir):
    """Repos holding this model's custom modelling code, from config.json auto_map.

    A trust_remote_code model does not necessarily carry its own code -- auto_map may
    point every class at a SEPARATE repo. Fetching only the weights then leaves the
    loader reaching for the Hub at first use, which fails closed under HF_HUB_OFFLINE
    and defeats the point of baking. So follow the references.
    """
    out = set()
    cfg = os.path.join(snapshot_dir, "config.json")
    if not os.path.exists(cfg):
        return out
    with open(cfg, encoding="utf-8") as handle:
        auto_map = json.load(handle).get("auto_map") or {}
    for target in auto_map.values():
        if isinstance(target, str) and "--" in target:
            out.add(target.split("--", 1)[0])
        elif isinstance(target, (list, tuple)):
            for item in target:
                if isinstance(item, str) and "--" in item:
                    out.add(item.split("--", 1)[0])
    return out


def main():
    selected = (os.environ.get("AIMEE_EMBEDDER") or "").strip()
    with open(REGISTRY, encoding="utf-8") as handle:
        table = json.load(handle)["embedders"]

    # An unknown name is a BUILD failure, not a silently-empty image: a container with
    # no weights starts fine and then cannot embed, which surfaces as a retrieval
    # outage rather than a build error. `none` is not valid here either -- that
    # deployment runs no embedder container at all.
    if selected not in table:
        sys.exit(f"AIMEE_EMBEDDER={selected!r} is not in the registry. "
                 f"Known: {', '.join(sorted(table))}. "
                 f"(An external embedder deploys no embedder container.)")

    spec = table[selected]
    repo, revision = spec["repo"], spec.get("revision") or "main"
    print(f"baking {selected}: {repo}@{revision}", flush=True)
    local = fetch(repo, revision=revision, ignore_patterns=SKIP)
    # Code repos are referenced by name only -- auto_map carries no revision -- so these
    # take the default branch. A looser pin than the weights get; a model whose code must
    # be pinned needs its auto_map repo added to the registry explicitly.
    for code_repo in sorted(code_repos(local)):
        print(f"  + code: {code_repo}", flush=True)
        fetch(code_repo, allow_patterns=["*.py", "*.json", "*.txt"])


if __name__ == "__main__":
    main()
