#!/usr/bin/env python3
"""Columns the store can read but nothing can write.

server_sessions.title was found this way: four operations return it, no
operation accepts it, and no statement inserts or updates it -- so it holds its
empty default forever and the user-facing search over it can never match. The C
module had the same shape, writing a literal '' on insert, so it is inherited
rather than dropped in translation.

A column like that is worse than unused. It has readers, so it looks live; it
has a default, so it never errors; and a test can construct the state by writing
the table directly and pass forever while no production path can produce it.

WHAT COUNTS AS WRITTEN: appearing in an INSERT column list, or on the left of an
UPDATE ... SET, in any family's SQL.

WHAT IS EXCLUDED, because a default is the intended value rather than an
absence:
  - defaults that compute something (now(), nextval, gen_random_uuid)
  - primary keys and generated identities
  - columns a CHECK constrains to a fixed set where the default is a real state

Reports suspects, not defects. Deciding whether a column should gain a writer or
be removed needs to know what it is for.
"""

import glob
import re
import sys
from collections import defaultdict

SCHEMA = "server-go/modules/aimee/families/*.sql"
FAMILIES = "server-go/modules/aimee/families/*.go"

# A default that produces a real value rather than a stand-in for "unset".
COMPUTED = re.compile(r"now\(\)|nextval|gen_random_uuid|CURRENT_", re.I)


def schema_columns() -> dict[str, dict[str, str]]:
    """table -> {column: default-or-empty}, for columns with a constant default."""
    tables: dict[str, dict[str, str]] = defaultdict(dict)
    for path in sorted(glob.glob(SCHEMA)):
        table = None
        for line in open(path):
            m = re.match(r"\s*CREATE TABLE(?: IF NOT EXISTS)? (\w+)", line)
            if m:
                table = m.group(1)
                continue
            if table is None or line.strip().startswith("--"):
                continue
            if line.strip().startswith(")"):
                table = None
                continue
            m = re.match(r"\s+(\w+)\s+\w+", line)
            if not m:
                continue
            column = m.group(1)
            if column.upper() in {"PRIMARY", "UNIQUE", "CHECK", "FOREIGN", "CONSTRAINT"}:
                continue
            d = re.search(r"DEFAULT\s+([^,]+?)(?:\s+CHECK|\s*,|\s*$)", line, re.I)
            default = d.group(1).strip() if d else ""
            if default and COMPUTED.search(default):
                continue  # a computed default is the value, not an absence
            if "PRIMARY KEY" in line.upper():
                continue
            tables[table][column] = default
    return tables


def written_columns() -> dict[str, set[str]]:
    """table -> columns any family writes."""
    writes: dict[str, set[str]] = defaultdict(set)
    body = "\n".join(open(p).read() for p in sorted(glob.glob(FAMILIES)))

    for table, cols in re.findall(r"INSERT\s+INTO\s+(\w+)\s*\(([^)]*)\)", body, re.I | re.S):
        for c in re.findall(r"\w+", cols):
            writes[table].add(c)

    for table, sets in re.findall(r"UPDATE\s+(\w+)\s+SET\s+(.*?)(?:WHERE|RETURNING|`)", body,
                                 re.I | re.S):
        for c in re.findall(r"(\w+)\s*=", sets):
            writes[table].add(c)

    # SQL BUILT FROM COLUMN DESCRIPTORS, not written as a literal.
    #
    # The roundtable family declares its columns as col{name, kind, set} via
    # text()/num()/real_()/boolint(), where set=true means "the entity's update
    # writes this", and assembles the statement at run time. None of that is
    # visible to a regex over SQL strings, so without this every such column
    # read as unwritten -- fifteen of the first run's twenty-seven suspects were
    # this, and the number was wrong before it was checked.
    #
    # Recorded against EVERY table rather than the one the descriptor belongs
    # to, because the list does not name its table and guessing would be worse
    # than approximating. Over-counting writers can only shrink the suspect
    # list, which is the safe direction for a survey a human has to judge: it
    # risks missing a real one, never inventing one.
    declared: set[str] = set()
    for name in re.findall(r"\b(?:text|num|real_|boolint)\(\s*\"(\w+)\"\s*,\s*true\s*\)", body):
        declared.add(name)
    if declared:
        for table in list(writes):
            writes[table] |= declared
    return writes


def selected_columns() -> set[str]:
    """Column names any family SELECTs.

    Deliberately name-only rather than table-qualified: a SELECT list does not
    say which table a bare column came from, and guessing would turn this from
    a conservative filter into a wrong one. Over-counting readers here can only
    SHRINK the suspect list, which is the safe direction for a survey whose
    output a human has to judge.
    """
    body = "\n".join(open(p).read() for p in sorted(glob.glob(FAMILIES)))
    names: set[str] = set()
    for select in re.findall(r"SELECT\s+(.*?)\s+FROM", body, re.I | re.S):
        for c in re.findall(r"\b([a-z_][a-z0-9_]*)\b", select):
            names.add(c)
    return names


def main() -> int:
    tables, writes = schema_columns(), written_columns()
    if not tables:
        print("survey-unwritten-columns: no tables parsed; the schema shape moved")
        return 2
    read = selected_columns()

    # Two populations, and only one of them is interesting.
    #
    #   read but not written -- a column with an audience. Every reader gets the
    #     default forever, so a feature built on it silently does nothing. This
    #     is server_sessions.title's shape and the reason for the script.
    #   neither read nor written -- an unused column. Untidy, harmless, and a
    #     different conversation.
    live, unused = [], 0
    for table in sorted(tables):
        if table not in writes:
            continue  # nothing writes this table at all; a different question
        for column, default in sorted(tables[table].items()):
            if column in writes[table]:
                continue
            if column in read:
                live.append((table, column, default or "(none)"))
            else:
                unused += 1

    for table, column, default in live:
        print(f"{table}.{column:28s} read but never written, default {default}")
    print(
        f"\n{len(live)} column(s) with readers and no writer"
        f"  ({unused} more are neither read nor written)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
