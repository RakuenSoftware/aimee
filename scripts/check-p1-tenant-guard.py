#!/usr/bin/env python3
"""check-p1-tenant-guard.py (N1): every exported function in the tenant-scoped db2
modules must call db2_tenant_require_pg() before touching the DB. This makes the
"every tenant entry is guarded" property falsifiable at build time — a new
entrypoint added without the guard fails CI, so it can never silently run
unprotected on the RLS-less SQLite shim.

Exit 0 if all guarded; exit 1 (listing offenders) otherwise.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODULES = [
    "src/modules/db2/c/team.c",
    "src/modules/db2/c/project.c",
    "src/modules/db2/c/membership.c",
    "src/modules/db2/c/admin_grant.c",
    "src/modules/db2/c/oidc_jwks.c",
]

# A top-level function definition: return-type + name(...) { at column 0, where the
# name starts with db2_ (the exported tenant CRUD entrypoints).
FUNC_RE = re.compile(r"^(?:[A-Za-z_][\w \*]*?)\b(db2_[A-Za-z0-9_]+)\s*\([^;]*\)\s*$", re.M)
GUARD = "db2_tenant_require_pg("


def function_bodies(src: str):
    """Yield (name, body) for each top-level db2_* function definition. The body is
    the text from this signature up to the next top-level signature (or EOF) — a
    slice that robustly contains only this function's statements without relying on
    brace matching."""
    matches = list(FUNC_RE.finditer(src))
    for idx, m in enumerate(matches):
        name = m.group(1)
        start = m.end()
        end = matches[idx + 1].start() if idx + 1 < len(matches) else len(src)
        yield name, src[start:end]


def main() -> int:
    offenders = []
    checked = 0
    for rel in MODULES:
        path = ROOT / rel
        src = path.read_text()
        found_any = False
        for name, body in function_bodies(src):
            found_any = True
            checked += 1
            if GUARD not in body:
                offenders.append(f"{rel}: {name}() missing {GUARD})")
        if not found_any:
            offenders.append(f"{rel}: no db2_* functions found (module renamed?)")
    if offenders:
        print("P1 tenant-guard check FAILED:")
        for o in offenders:
            print("  -", o)
        return 1
    print(f"P1 tenant-guard check OK: {checked} tenant entrypoints all call {GUARD})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
