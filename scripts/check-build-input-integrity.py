#!/usr/bin/env python3
"""Reject executable network downloads lacking an adjacent digest check."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
candidates = [*root.glob("Dockerfile*"), *(root / "deploy").rglob("Dockerfile*")]
bad = []
for path in candidates:
    lines = path.read_text(errors="replace").splitlines()
    for i, line in enumerate(lines):
        if re.search(r"\b(curl|wget)\b", line) and re.search(r"https?://", line):
            if re.search(r"https?://(?:127\.0\.0\.1|localhost)(?:[:/])", line):
                continue
            window = "\n".join(lines[i:min(len(lines), i + 8)])
            if not re.search(r"sha(256|512)sum\s+-c|cosign\s+verify", window):
                bad.append(f"{path.relative_to(root)}:{i + 1}")
if bad:
    print("build-input-integrity-check: unverified downloads:\n" + "\n".join(bad), file=sys.stderr)
    raise SystemExit(1)
print("build-input-integrity-check: executable downloads have digest/signature verification")
