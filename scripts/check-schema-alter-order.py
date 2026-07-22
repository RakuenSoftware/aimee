#!/usr/bin/env python3
"""Guard that every ALTER TABLE in a schema file comes AFTER that table's CREATE.

schema.sql is applied top-to-bottom to a FRESH database. An `ALTER TABLE t ADD
COLUMN IF NOT EXISTS ...` ordered before `CREATE TABLE IF NOT EXISTS t` does not
skip -- IF NOT EXISTS guards the COLUMN, not the TABLE -- so it raises
"relation t does not exist" and aborts the whole schema apply. The server then
comes up with no schema at all.

This is invisible on an already-migrated database, where the table exists and the
ALTER is a harmless no-op. It only bites a fresh install, so the only thing that
caught it was the docker e2e -- ten minutes after the fact. The check is a
two-second string scan; there is no reason to pay for it in CI minutes.

Real case: next_attempt_at's ALTER landed at line 25 while kb_async_jobs was
created at line 118, taking down T1/T2 e2e-docker on a fresh postgres.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMAS = [ROOT / "src" / "db2" / "schema.sql", ROOT / "src" / "db2" / "schema_sqlite.sql"]

CREATE_RE = re.compile(r"^\s*CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([a-zA-Z_][a-zA-Z0-9_]*)", re.I)
ALTER_RE = re.compile(r"^\s*ALTER\s+TABLE\s+(?:IF\s+EXISTS\s+)?([a-zA-Z_][a-zA-Z0-9_]*)", re.I)


def check(path: Path) -> list[str]:
    if not path.exists():
        return []
    created: dict[str, int] = {}
    problems = []
    for n, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
        m = CREATE_RE.match(line)
        if m:
            created.setdefault(m.group(1).lower(), n)
            continue
        m = ALTER_RE.match(line)
        if m:
            t = m.group(1).lower()
            if t not in created:
                where = "never created in this file"
                # A table created LATER is the ordering bug; one never created here
                # may legitimately live in another schema file.
                for n2, l2 in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
                    m2 = CREATE_RE.match(l2)
                    if m2 and m2.group(1).lower() == t:
                        where = f"created later at line {n2}"
                        break
                else:
                    continue  # not created here at all -- not this check's business
                problems.append(f"  {path.name}:{n}: ALTER TABLE {t} but the table is {where}")
    return problems


def main() -> int:
    problems = [p for s in SCHEMAS for p in check(s)]
    if problems:
        print("check-schema-alter-order: FAIL — an ALTER runs before its CREATE. On a fresh\n"
              "database this raises \"relation ... does not exist\" and aborts the entire schema\n"
              "apply (IF NOT EXISTS guards the column, not the table):")
        print("\n".join(problems))
        print("Move each ALTER below its CREATE TABLE.")
        return 1
    print(f"check-schema-alter-order: ok ({len(SCHEMAS)} schema files, every ALTER follows its CREATE)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
