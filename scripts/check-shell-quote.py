#!/usr/bin/env python3
"""Keep shell construction on the quoted-token API and git path sink argv-only."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
src = root / "src"
bad = []
for path in src.rglob("*.c"):
    text = path.read_text(errors="replace")
    if "shell_escape(" in text:
        bad.append(f"{path.relative_to(root)}: obsolete shell_escape()")
git = (src / "modules/git/mcp_git_query.c").read_text()
if re.search(r"(?:safe_exec_capture|popen|system)\s*\([^;]*git -C", git, re.S):
    bad.append("src/modules/git/mcp_git_query.c: git path reached a shell-backed sink")
if bad:
    print("shell-quote-check: failed:\n" + "\n".join(bad), file=sys.stderr)
    raise SystemExit(1)
print("shell-quote-check: quoted-token API and argv-only git sink verified")
