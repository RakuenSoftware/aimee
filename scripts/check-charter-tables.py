#!/usr/bin/env python3
"""Enforce the charter artifact-table invariant.

The architecture charter defines exactly four shared tables —
artifacts, artifact_citations, artifact_links, audit_events — created
exactly once, in DB2 (src/modules/db2/c/schema.sql). The cross-source-learning and
deep-curator proposals both write into them and must NOT introduce parallel
artifact/citation/audit tables of their own.

This check fails if:
  * any charter table is missing or declared more than once in the DB2 schema;
  * a charter table is also declared in the DB1 schema (wrong tier); or
  * a rogue table whose name looks like a parallel artifact/audit store
    (e.g. *_artifacts, learning_audit_events, curator_artifact_links) is
    declared anywhere in the schemas.

See docs/proposals/accepted/cross-source-learning-substrate.md (AC: "No new
artifact / citation / audit tables outside the charter schema").
"""

import argparse
import pathlib
import re
import sys
import tempfile

CHARTER_TABLES = ("artifacts", "artifact_citations", "artifact_links", "audit_events")

# Sanctioned per-service WORM audit stores (the auditable-worm-audit-store
# initiative, docs/proposals/pending/auditable-worm-audit-store.md): append-only,
# hash-chained, tamper-evident stores that are DELIBERATELY separate from the
# charter audit_events — they are the audit-of-record, not a parallel artifact
# store. Exempt from the rogue-store check.
SANCTIONED_AUDIT_TABLES = ("kb_audit_event",)

CREATE_RE = re.compile(r"CREATE TABLE IF NOT EXISTS\s+([a-z_][a-z0-9_]*)", re.IGNORECASE)

# A non-charter table is "rogue" if its name embeds an artifact/citation/audit
# role word — that is exactly the parallel-store shape the charter forbids.
ROGUE_RE = re.compile(r"(artifact|audit_event|artifact_citation|artifact_link)", re.IGNORECASE)


def tables_in(path: pathlib.Path):
    """Tables declared by a schema file, or by a DIRECTORY of schema files.

    The store's schema stopped being one file when it became a Go module: it is
    now 21 per-family files under server-go/modules/aimee/families/. This used
    to name src/modules/db1/schema.sql, and when that was deleted with the C
    module `tables_in` returned [] for it -- so half of this check went quiet.
    The "charter table must live in DB2 only" rule and the rogue-table scan over
    the store's tables were both examining nothing, and the check still passed.

    A missing path is now an ERROR rather than an empty list, because every
    caller here is naming a schema it expects to exist.
    """
    if path.is_dir():
        text = "\n".join(f.read_text() for f in sorted(path.glob("*.sql")))
        if not text:
            raise SystemExit(f"check-charter-tables: {path} holds no .sql files; "
                             f"this check would pass having read nothing")
        return CREATE_RE.findall(text)
    if not path.exists():
        raise SystemExit(f"check-charter-tables: {path} does not exist; this check "
                         f"would pass having read nothing")
    return CREATE_RE.findall(path.read_text())


def check(db2_schema: pathlib.Path, db1_schema: pathlib.Path) -> int:
    db2_tables = tables_in(db2_schema)
    db1_tables = tables_in(db1_schema)
    errors = []

    for t in CHARTER_TABLES:
        n = db2_tables.count(t)
        if n == 0:
            errors.append(f"charter table '{t}' missing from {db2_schema}")
        elif n > 1:
            errors.append(f"charter table '{t}' declared {n} times in {db2_schema} (must be once)")
        if t in db1_tables:
            errors.append(f"charter table '{t}' must live in DB2 only, found in {db1_schema}")

    for t in set(db2_tables) | set(db1_tables):
        if t in CHARTER_TABLES or t in SANCTIONED_AUDIT_TABLES:
            continue
        if ROGUE_RE.search(t):
            errors.append(
                f"rogue table '{t}' looks like a parallel artifact/audit store; "
                "write into the charter tables instead"
            )

    if errors:
        for e in errors:
            print(f"charter-tables: ERROR {e}", file=sys.stderr)
        return 1

    print(f"charter-tables: ok ({len(CHARTER_TABLES)} charter tables pinned, no rogue stores)")
    return 0


def plant_test() -> int:
    """Inject a rogue table and confirm the check rejects it."""
    with tempfile.TemporaryDirectory() as d:
        db2 = pathlib.Path(d) / "schema.sql"
        db1 = pathlib.Path(d) / "db1.sql"
        db1.write_text("CREATE TABLE IF NOT EXISTS foo (id BIGINT);\n")
        good = "".join(f"CREATE TABLE IF NOT EXISTS {t} (id TEXT);\n" for t in CHARTER_TABLES)
        # Baseline (clean) must pass.
        db2.write_text(good)
        if check(db2, db1) != 0:
            print("charter-tables: plant-test FAIL (clean schema rejected)", file=sys.stderr)
            return 1
        # A parallel store must be rejected.
        db2.write_text(good + "CREATE TABLE IF NOT EXISTS learning_artifacts (id TEXT);\n")
        if check(db2, db1) == 0:
            print("charter-tables: plant-test FAIL (rogue table not caught)", file=sys.stderr)
            return 1
        # A duplicate charter table must be rejected.
        db2.write_text(good + "CREATE TABLE IF NOT EXISTS audit_events (id TEXT);\n")
        if check(db2, db1) == 0:
            print("charter-tables: plant-test FAIL (duplicate table not caught)", file=sys.stderr)
            return 1
    print("charter-tables: plant-test ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Enforce the charter artifact-table invariant.")
    parser.add_argument(
        "--src-dir", default="src",
        help="Source directory containing modules/db2/c/schema.sql",
    )
    parser.add_argument("--plant-test", action="store_true", help="Run an internal self-test")
    args = parser.parse_args()

    if args.plant_test:
        return plant_test()

    # Resolved: the make target passes --src-dir . from src/, and Path(".").parent
    # is "." rather than the repository root.
    src = pathlib.Path(args.src_dir).resolve()
    return check(
        src / "modules" / "db2" / "c" / "schema.sql",
        # The store's schema, one file per family since it became a Go module.
        src.parent / "server-go" / "modules" / "aimee" / "families",
    )


if __name__ == "__main__":
    raise SystemExit(main())
