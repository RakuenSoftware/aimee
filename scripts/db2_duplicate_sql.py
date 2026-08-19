"""Report DB2 backend functions that issue identical SQL.

Two symbols running the same statements are one operation wearing two names. The
DB2 bus migration gives each reviewed declaration a wire operation, so a pair
that goes unnoticed becomes two identical operations on the same stage, told
apart by nothing, and no test can see that they are the same. Both duplicate
pairs found so far were exactly that, and both differed only in how they
reported failure -- which decided which implementation the boundary binds.

Run before reviewing a declaration:

    python3 scripts/db2_duplicate_sql.py

Exit status is 0 whether or not duplicates exist; this reports, it does not
gate. A duplicate is not automatically wrong -- it needs a person to decide
whether to fold the pair onto one operation or keep them apart for a reason
worth writing down.
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path("src/modules/db2/c")
DEFINITION = re.compile(
    r"^(?:int64_t|int|void|double|char\s*\*|const\s+char\s*\*)\s+(\w+)\s*\([^;]*?\)\s*\{", re.M)
SQL_VERB = re.compile(r"\b(SELECT|INSERT|UPDATE|DELETE)\b", re.I)
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def _body(text: str, start: int) -> str:
    depth, index = 0, start
    while index < len(text):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index]
        index += 1
    return text[start:]


def statements(body: str) -> str:
    """Concatenated string literals, whitespace-normalised.

    These statements are written as adjacent literals split across lines, so
    where the line happens to wrap is not a difference worth reporting.
    """
    return re.sub(r"\s+", " ", " ".join(LITERAL.findall(body))).strip()


def main() -> int:
    if not ROOT.is_dir():
        print(f"{ROOT} not found; run from the repository root", file=sys.stderr)
        return 2

    by_sql = defaultdict(list)
    for path in sorted(ROOT.glob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in DEFINITION.finditer(text):
            body = _body(text, text.index("{", match.end() - 1))
            sql = statements(body)
            if sql and SQL_VERB.search(sql):
                by_sql[sql].append((match.group(1), path.name))

    groups = sorted(((sql, members) for sql, members in by_sql.items() if len(members) > 1),
                    key=lambda item: -len(item[1]))
    for sql, members in groups:
        print(f"{len(members)} symbols run the same statements:")
        for symbol, filename in members:
            print(f"    {symbol}  ({filename})")
        print(f"    {sql[:160]}")
        print()

    redundant = sum(len(members) - 1 for _sql, members in groups)
    print(f"{len(groups)} group(s) of identical SQL; {redundant} redundant symbol(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
