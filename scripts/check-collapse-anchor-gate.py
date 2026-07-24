#!/usr/bin/env python3
"""Require the complete Phase 0 anchor contract before collapse implementation."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
anchors = root / "docs/guardrails/collapse_anchors.md"
if not anchors.is_file():
    sys.exit("error: docs/guardrails/collapse_anchors.md is required before Phase 1+")
text = anchors.read_text(encoding="utf-8")
missing = [str(n) for n in range(1, 7) if f"Decision {n}" not in text]
if missing:
    sys.exit("error: collapse anchor decisions missing: " + ", ".join(missing))
print("collapse anchor gate: OK (six decisions present)")
