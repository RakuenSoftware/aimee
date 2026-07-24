#!/usr/bin/env python3
"""Enforce the Phase 0 anchor contract for changes to Phase 1+ surfaces."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ANCHORS = "docs/guardrails/collapse_anchors.md"
TOUCHED = (
    "src/server/", "src/modules/guardrails/", "src/headers/aimee_ir.h",
    "src/modules/config/", "src/modules/audit/",
    "scripts/check-collapse-anchor-gate.py",
)

def changed_paths():
    try:
        out = subprocess.check_output(["git", "diff", "--name-only", "HEAD~1", "HEAD"], cwd=ROOT, text=True)
    except (OSError, subprocess.CalledProcessError):
        return []
    return [line for line in out.splitlines() if line]

def main():
    paths = changed_paths()
    phase_one_touched = any(path.startswith(TOUCHED) for path in paths)
    if not phase_one_touched:
        print("collapse anchor gate: OK (no Phase 1+ paths changed)")
        return 0
    anchor = ROOT / ANCHORS
    if not anchor.is_file():
        print(f"error: {ANCHORS} is required when Phase 1+ paths change", file=sys.stderr)
        return 1
    text = anchor.read_text(encoding="utf-8")
    missing = [str(n) for n in range(1, 7) if f"Decision {n}" not in text]
    if missing:
        print("error: collapse anchor decisions missing: " + ", ".join(missing), file=sys.stderr)
        return 1
    if ANCHORS not in paths:
        print(f"error: {ANCHORS} must be included when Phase 1+ paths change", file=sys.stderr)
        return 1
    print("collapse anchor gate: OK (Phase 1+ diff includes six decisions)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
