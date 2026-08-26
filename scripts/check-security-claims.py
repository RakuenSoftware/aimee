#!/usr/bin/env python3
from pathlib import Path
import json
import sys

root = Path(__file__).resolve().parents[1]
registry = json.loads((root / "docs/security-claims.json").read_text())
security = (root / "docs/SECURITY.md").read_text()
required = {"id", "artifacts", "default", "boundary", "owner", "tests", "evidence", "limitations"}
seen = set()
errors = []
for claim in registry.get("claims", []):
    missing = required - claim.keys()
    if missing:
        errors.append(f"{claim.get('id', '<unknown>')}: missing {sorted(missing)}")
        continue
    cid = claim["id"]
    if cid in seen or cid not in security:
        errors.append(f"{cid}: duplicate or absent from docs/SECURITY.md")
    seen.add(cid)
    for test in claim["tests"]:
        if not (root / test).is_file():
            errors.append(f"{cid}: missing evidence test {test}")
if errors:
    print("security-claims-check: failed\n" + "\n".join(errors), file=sys.stderr)
    raise SystemExit(1)
print(f"security-claims-check: {len(seen)} release-qualified claims")
