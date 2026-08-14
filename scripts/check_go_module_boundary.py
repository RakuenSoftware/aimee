#!/usr/bin/env python3
"""Forbid one Go module implementation from importing another."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / "server-go" / "modules"
PREFIX = "github.com/JBailes/aimee/server-go/modules/"
# Closed by design. Peer modules communicate over the event bus.
ALLOWLIST: set[tuple[str, str]] = set()
IMPORT = re.compile(r'["`](' + re.escape(PREFIX) + r'[^"`]+)["`]')


def violations(root: Path = MODULES) -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    for source in sorted(root.rglob("*.go")):
        try:
            relative = source.relative_to(ROOT).as_posix()
        except ValueError:
            relative = source.relative_to(root).as_posix()
        owner = source.relative_to(root).parts[0]
        for imported in IMPORT.findall(source.read_text(encoding="utf-8")):
            peer = imported[len(PREFIX):].split("/", 1)[0]
            if peer != owner and (relative, imported) not in ALLOWLIST:
                found.append((relative, imported))
    return found


def main() -> int:
    found = violations()
    for source, imported in found:
        print(f"{source}: imports peer module {imported}", file=sys.stderr)
    if found:
        print(f"check_go_module_boundary: {len(found)} violation(s)", file=sys.stderr)
        return 1
    print("check_go_module_boundary: ok (empty allowlist)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
