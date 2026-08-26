#!/usr/bin/env python3
"""Reject remotely executed GitHub Actions that are not pinned to a commit."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
bad = []
for path in sorted((root / ".github/workflows").glob("*.y*ml")):
    for number, line in enumerate(path.read_text().splitlines(), 1):
        match = re.search(r"\buses:\s*([^\s#]+)", line)
        if not match or match.group(1).startswith("./"):
            continue
        ref = match.group(1).rsplit("@", 1)[-1]
        if not re.fullmatch(r"[0-9a-fA-F]{40}", ref):
            bad.append(f"{path.relative_to(root)}:{number}: {match.group(1)}")
if bad:
    print("workflow-pin-check: mutable action references:\n" + "\n".join(bad), file=sys.stderr)
    raise SystemExit(1)
print("workflow-pin-check: all remote actions are commit-pinned")
