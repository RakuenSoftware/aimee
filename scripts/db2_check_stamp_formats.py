"""Wall-clock stamps written in a different format than the C writes them.

DB2 stores timestamps as TEXT, and the schema is explicit that there is ONE
canonical format -- ISO 8601, 'YYYY-MM-DDTHH24:MI:SSZ', which pg_now_text()
returns. The reason is not cosmetic: these columns are compared AS TEXT against
pg_now_text() in dozens of places, and 'T' (0x54) sorts above ' ' (0x20), so a
row stamped with a space separator compares backwards against a threshold on
the same date. Sweeps then skip or collect a day's worth of rows silently.

The C is not consistent about it -- most sites emit ISO, a handful still emit
the space form -- so "use ISO everywhere" and "write what the C writes" are
different instructions, and only one of them keeps the parity run meaningful.
This pairs the two so the choice is made per site with the C in view rather
than from memory.

The pairing is by table AND column, not by column. decided_at is written ISO on
bandit_decisions and with a space on ontology_evaluations; pairing by name
alone reported the correct one of those as a defect and would have had it
"fixed" into a real divergence.

The parity run found two of these by noticing a reply was one byte longer than
the C's. That only works for columns some reply echoes; this covers the rest.
"""
import re
import sys
from pathlib import Path

C_DIR = Path("src/modules/db2/c")
MODULE = Path("server-go/modules/db2")

ISO = "iso"
SPACE = "space"

# The two spellings, in the forms both sides write them.
SPELLINGS = [
    (ISO, re.compile(r"pg_now_text\(\)")),
    (ISO, re.compile(r"'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'")),
    (ISO, re.compile(r"%Y-%m-%dT%H:%M:%SZ")),
    (SPACE, re.compile(r"'YYYY-MM-DD HH24:MI:SS'")),
    (SPACE, re.compile(r"%Y-%m-%d %H:%M:%S")),
]

# A column assigned a stamp, in SQL either side embeds. Also the bound form,
# `column = ?n` / `column = $n`, where the format lives in whatever filled the
# parameter -- those are attributed from the file's own stamp helper.
ASSIGNMENT = re.compile(
    r"(\w+)\s*=\s*(to_char\([^)]*\)|pg_now_text\([^)]*\)|[?$]\d+)")
# `DO UPDATE SET` is not a table named "set": the exclusion keeps an upsert
# attributed to the table it inserts into.
TABLE = re.compile(r"(?:UPDATE|INSERT\s+INTO)\s+(?!SET\b)(\w+)", re.I)

# Columns that hold a wall-clock stamp. A bound parameter tells us nothing
# about what it carries, so only these are attributed from the file helper.
STAMP_COLUMN = re.compile(r"_at$|^ts$|_time$")


# Differences from the C that were decided rather than overlooked.
#
# The C stamps these with to_char(CURRENT_TIMESTAMP, ...), which renders in the
# session's timezone, while pg_now_text() renders UTC. Nothing compares these
# two columns as text against a pg_now_text() threshold, so the spelling is not
# load-bearing for any sweep -- but a column holding local time for rows one
# writer wrote and UTC for rows another wrote cannot be compared row to row at
# all, which is worse than either spelling alone. The port writes UTC in both.
#
# Entries are keyed (table, column) and carry the reason, so an unexplained
# difference is still a finding.
DELIBERATE = {
    ("entity_nodes", "updated_at"):
        "the C renders in the session timezone; this writes UTC like every "
        "other stamp in the table",
    ("entity_edges", "updated_at"):
        "same as entity_nodes.updated_at -- the edge and the node it joins are "
        "stamped by the same sweep and must be comparable",
}


def spelling(text: str) -> str | None:
    """Which of the two formats a fragment writes, if either."""
    for name, pattern in SPELLINGS:
        if pattern.search(text):
            return name
    return None


def file_stamp_spelling(text: str) -> str | None:
    """The format a source file's own stamp helper emits, if it has one."""
    # now_utc() is the process-wide helper aimee.h declares, and it has always
    # emitted ISO -- it is the reason ISO is the canonical format.
    iso = (bool(re.search(r"%Y-%m-%dT%H:%M:%SZ", text)) or "db2_now_utc" in text
           or bool(re.search(r"\bnow_utc\s*\(", text)))
    space = bool(re.search(r"%Y-%m-%d %H:%M:%S", text))
    if iso and space:
        return "mixed"
    if iso:
        return ISO
    if space:
        return SPACE
    return None


def statements(paths) -> list:
    """(source, table, column, spelling) for every stamped column assignment."""
    found = []
    for path in paths:
        text = path.read_text()
        helper = file_stamp_spelling(text)
        for match in ASSIGNMENT.finditer(text):
            column, expression = match.group(1), match.group(2)
            name = spelling(expression)
            if name is None:
                # A bound parameter: attributable only when the column is a
                # stamp and the file has exactly one way of making one.
                if not STAMP_COLUMN.search(column) or helper in (None, "mixed"):
                    continue
                name = helper
            # The nearest UPDATE/INSERT above the assignment names the table.
            preceding = TABLE.findall(text[:match.start()])
            table = preceding[-1].lower() if preceding else "?"
            found.append((path.name, table, column, name))
    return found


def main() -> int:
    c_sql = statements(sorted(C_DIR.glob("*.c")))
    go_sql = statements(sorted(p for p in MODULE.glob("*.go")
                               if not p.name.endswith("_test.go")))

    c_by_target: dict = {}
    for _, table, column, name in c_sql:
        c_by_target.setdefault((table, column), set()).add(name)

    print(f"C: {len(c_sql)} stamped assignments over {len(c_by_target)} "
          f"table/column pair(s)")
    print(f"Go: {len(go_sql)} stamped assignments")

    disagreements, ambiguous, unpaired = [], [], []
    for source, table, column, name in go_sql:
        expected = c_by_target.get((table, column))
        if not expected:
            unpaired.append((source, table, column, name))
        elif len(expected) > 1:
            ambiguous.append((source, table, column, name, sorted(expected)))
        elif name not in expected and (table, column) not in DELIBERATE:
            disagreements.append((source, table, column, name, sorted(expected)))

    print(f"\n{len(disagreements)} stamp(s) written in a format the C does not "
          f"write there:")
    for source, table, column, name, expected in sorted(disagreements):
        print(f"   {source:28s} {table}.{column:22s} go={name:6s} "
              f"c={','.join(expected)}")

    if ambiguous:
        print(f"\n{len(ambiguous)} the C writes both ways -- decide per site:")
        for source, table, column, name, expected in sorted(ambiguous):
            print(f"   {source:28s} {table}.{column:22s} go={name}")

    # A stamp can also sit in a value position -- inside VALUES(...), in the
    # select list of an INSERT ... SELECT, or in a CASE arm -- where no
    # `column =` precedes it and nothing pairs it with a column. Three of those
    # were left in the wrong format by a run that reported no disagreements, so
    # they are checked too, at the coarser granularity available: the table
    # being written and the spellings the C uses in value positions on it.
    def value_position(paths):
        found = []
        for path in sorted(paths):
            if path.name.endswith("_test.go"):
                continue
            text = path.read_text()
            for match in re.finditer(r"(to_char\([^)]*\)|pg_now_text\(\))", text):
                if re.search(r"\w+\s*=\s*$",
                             text[max(0, match.start() - 40):match.start()]):
                    continue
                name = spelling(match.group(1))
                if not name:
                    continue
                preceding = TABLE.findall(text[:match.start()])
                found.append((path.name,
                              preceding[-1].lower() if preceding else "?", name))
        return found

    c_positional: dict = {}
    for _, table, name in value_position(C_DIR.glob("*.c")):
        c_positional.setdefault(table, set()).add(name)

    # Where the C stamps a table only in assignment position, those spellings
    # still say what the table holds, so they stand in rather than leaving the
    # site unchecked.
    c_by_table: dict = {}
    for _, table, _, name in c_sql:
        c_by_table.setdefault(table, set()).add(name)

    positional, positional_unpaired = [], []
    for source, table, name in value_position(MODULE.glob("*.go")):
        expected = c_positional.get(table) or c_by_table.get(table)
        if not expected:
            positional_unpaired.append((source, table, name))
        elif (len(expected) == 1 and name not in expected
              and not any(t == table for t, _ in DELIBERATE)):
            positional.append((source, table, name, sorted(expected)))

    print(f"\n{len(positional)} stamp(s) in a value position written in a format "
          f"the C does not write on that table:")
    for source, table, name, expected in sorted(positional):
        print(f"   {source:28s} {table:30s} go={name:6s} c={','.join(expected)}")

    if positional_unpaired:
        print(f"\n{len(positional_unpaired)} in a value position on a table the C "
              f"does not stamp that way -- check by hand:")
        for source, table, name in sorted(positional_unpaired):
            print(f"   {source:28s} {table:30s} {name}")

    if unpaired:
        print(f"\n{len(unpaired)} with no C statement writing the same column:")
        for source, table, column, name in sorted(unpaired):
            print(f"   {source:28s} {table}.{column:22s} go={name}")

    return 1 if disagreements or positional else 0


if __name__ == "__main__":
    sys.exit(main())
