#!/usr/bin/env python3
"""No table may be declared in two of the store's schema files.

Every file says CREATE TABLE IF NOT EXISTS, so a table declared twice does not
error -- the first file to apply wins and the second is a silent no-op. Which
one wins depends on the order the files are applied in, and nothing in the
per-family SQL suites can see the conflict: each suite applies only its own
family's schema, so each meets only its own definition and passes.

agent_log was declared in schema_agent_work.sql and schema_delegation.sql with
different types for `success` (BOOLEAN vs BIGINT) and a different constraint on
`confidence`. The reader in the second family scanned the column as an integer,
which the winning definition is not, so that operation could not have worked --
and every suite was green.

One table, one owner, one declaration. A family that only READS another
family's table does not declare it. The reviewed memory send-guard upgrade
reapplies identical CREATE definitions in its own append-only migration; its
original migration checksum must remain unchanged.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA_DIR = ROOT / "server-go" / "modules" / "aimee" / "families"

CREATE_RE = re.compile(
    r"create\s+table\s+(?:if\s+not\s+exists\s+)?(?:\"([^\"]+)\"|([a-z_][a-z0-9_]*))",
    re.IGNORECASE,
)


def main() -> int:
    files = sorted(SCHEMA_DIR.glob("schema_*.sql"))
    if not files:
        print(f"check-schema-duplicates: no schema files under {SCHEMA_DIR}", file=sys.stderr)
        return 1

    owners: dict[str, list[str]] = {}
    definitions: dict[tuple[str, str], list[tuple[str, ...]]] = {}
    for path in files:
        text = re.sub(r"--[^\n]*", "", path.read_text())
        for m in CREATE_RE.finditer(text):
            name = (m.group(1) or m.group(2)).lower()
            owners.setdefault(name, []).append(path.name)
            end = text.find(";", m.end())
            statement = text[m.start():end + 1] if end >= 0 else ""
            tokens = tuple(re.findall(r"'(?:''|[^'])*'|[a-zA-Z_][a-zA-Z_0-9]*|[^\s]", statement))
            definitions.setdefault((name, path.name), []).append(tokens)

    duplicates = {t: sorted(set(f)) for t, f in owners.items() if len(set(f)) > 1}
    # Both files belong to the same private memory owner. Permit only this
    # exact migration pair, once per file, with identical complete CREATE SQL.
    # An altered definition or a third declaring file still fails the gate.
    reapply = {"schema_personal_memory_send_guards.sql",
               "schema_personal_memory_send_guard_completion.sql"}
    for table in ("memory_send_barrier", "memory_send_leases"):
        if set(duplicates.get(table, ())) == reapply:
            original, upgrade = [definitions[(table, name)] for name in sorted(reapply)]
            if len(original) == len(upgrade) == 1 and original == upgrade and original[0]:
                del duplicates[table]
    if duplicates:
        print("schema drift: a table is declared in more than one file.")
        print("Both files say IF NOT EXISTS, so the first to apply wins in silence:")
        for table, where in sorted(duplicates.items()):
            print(f"  {table}: {', '.join(where)}")
        print()
        print("One table, one owner. A family that only reads another family's")
        print("table must not declare it.")
        return 1

    print(f"check-schema-duplicates: ok ({len(owners)} tables across {len(files)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
