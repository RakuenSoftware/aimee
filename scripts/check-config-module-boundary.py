#!/usr/bin/env python3
"""Enforce the extracted pure-Go configuration ownership boundary."""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
FORBIDDEN = ("config_" + "t", "config_" + "load")
EXTERNAL_MODULE = "github.com/RakuenSoftware/aimee-module-config"
FROZEN_EVIDENCE = (
    Path("benchmarks/fixtures/gemma4-unified/ab-v1"),
    Path("benchmarks/results/gemma4-unified/ab-v1"),
    Path("benchmarks/hashline/corpus.generated.json"),
)


def is_frozen_evidence(relative: Path) -> bool:
    """Published benchmark bytes are evidence, not executable repository code."""
    return any(relative == root or root in relative.parents for root in FROZEN_EVIDENCE)


def tracked_files() -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        check=True, stdout=subprocess.PIPE,
    )
    return [ROOT / item.decode() for item in result.stdout.split(b"\0") if item]


def main() -> int:
    failures: list[str] = []
    for path in tracked_files():
        relative = path.relative_to(ROOT)
        if ".ci-logs" in relative.parts or is_frozen_evidence(relative):
            continue
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for token in FORBIDDEN:
            if re.search(rf"\b{re.escape(token)}\b", text):
                failures.append(f"{relative} contains retired token {token!r}")

    native_root = ROOT / "src/modules/config"
    for suffix in ("*.c", "*.h", "*.cpp", "*.hpp"):
        for path in native_root.rglob(suffix):
            failures.append(f"native configuration implementation remains: {path.relative_to(ROOT)}")

    local_go = ROOT / "server-go/modules/config"
    if local_go.exists() and any(local_go.rglob("*")):
        failures.append("local Go configuration implementation remains under server-go/modules/config")

    native_boundaries = {
        ROOT / "src" / "cmd_profile.c": ("aimee.yaml", "fopen("),
        ROOT / "src" / "toolset.c": ("yaml_parse(", '#include "yaml.h"'),
    }
    for path, tokens in native_boundaries.items():
        text = path.read_text(encoding="utf-8")
        for token in tokens:
            if token in text:
                failures.append(
                    f"{path.relative_to(ROOT)} bypasses the config module with {token!r}"
                )
    if "config_default_path" in (ROOT / "src" / "config_client_contract.c").read_text(encoding="utf-8"):
        failures.append("native caller still exposes a configuration-file path")

    go_mod = (ROOT / "server-go/go.mod").read_text(encoding="utf-8")
    if EXTERNAL_MODULE not in go_mod:
        failures.append("server-go/go.mod does not pin the external configuration module")

    if failures:
        for failure in failures:
            print(f"config module boundary: {failure}", file=sys.stderr)
        return 1
    print("config module boundary: ok (external pure-Go owner; caller contracts only)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
