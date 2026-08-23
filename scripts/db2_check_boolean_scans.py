"""Boolean columns scanned into something that is not a bool.

pgx sends and receives typed parameters, so a BOOLEAN column scanned into an
int64 fails the whole read -- the operation answers "not found" for a row that
is there, which is what mining_job_get did until the parity run compared it.

This lists every BOOLEAN column in the schema and every Go statement that
selects one, so the pairing can be checked rather than remembered.
"""
import re
import sys
from pathlib import Path

SCHEMA = Path("src/modules/db2/c/schema.sql")
MODULE = Path("server-go/modules/db2")


def boolean_columns() -> dict:
    """table -> the columns declared BOOLEAN."""
    text = SCHEMA.read_text()
    found = {}
    for match in re.finditer(r"CREATE TABLE IF NOT EXISTS (\w+) \((.*?)\);", text, re.S):
        table, body = match.group(1), match.group(2)
        columns = [c.strip() for c in body.split(",")]
        names = [c.split()[0] for c in columns
                 if len(c.split()) > 1 and c.split()[1].upper().startswith("BOOLEAN")]
        if names:
            found[table] = names
    # ALTER TABLE ... ADD COLUMN ... BOOLEAN
    for match in re.finditer(
            r"ALTER TABLE (\w+) ADD COLUMN IF NOT EXISTS (\w+) BOOLEAN", text):
        found.setdefault(match.group(1), []).append(match.group(2))
    return found


def main() -> int:
    columns = boolean_columns()
    print(f"boolean columns in {len(columns)} table(s)")

    suspects = []
    for path in sorted(MODULE.glob("*.go")):
        if path.name.endswith("_test.go"):
            continue
        text = path.read_text()
        # Every raw string that opens a SELECT, not only the ones assigned
        # whole to a `somethingQuery` constant. Two of these statements are
        # built by concatenating a shared column list onto a WHERE clause, and
        # matching only the assignment form skipped both -- including the one
        # that reads docs.review_needed, a BOOLEAN column.
        for match in re.finditer(r"(\w+)?\s*=?\s*`(SELECT.*?)`", text, re.S):
            name = match.group(1) or "<unnamed>"
            statement = match.group(2)
            # Quoted text is data, not a column: 'active' and 'complete' are
            # values these statements compare against, and matching them made
            # the report noise rather than a finding.
            unquoted = re.sub(r"'[^']*'", "''", statement)
            selected = []
            for table, names in columns.items():
                if not re.search(rf"\b{table}\b", unquoted):
                    continue
                selected += [f"{table}.{c}" for c in names
                             if re.search(rf"\b{c}\b", unquoted)]
            if selected:
                suspects.append((path.name, name, sorted(set(selected))))

    print(f"{len(suspects)} read statement(s) select a boolean column:")
    for source, name, selected in suspects:
        print(f"   {source:34s} {name:38s} {','.join(selected)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
