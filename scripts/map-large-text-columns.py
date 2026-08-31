#!/usr/bin/env python3
"""Map db1's large text columns to their tables.

The catalog declares six reply fields at 1 MiB -- body, context, messages_json,
prompt, result, text -- and the store wire refuses a cell over that rather than
truncating it. The columns behind those fields are unbounded TEXT, so this
reports which table each lives in, to check that a write cannot store a value
the read path can never return.
"""

import glob
import re
import sys

TARGETS = {"prompt", "result", "messages_json", "text", "body", "context_json"}


def main() -> int:
    found = []
    for path in sorted(glob.glob("server-go/modules/aimee/families/*.sql")):
        table = None
        for line in open(path):
            m = re.match(r"\s*CREATE TABLE(?: IF NOT EXISTS)? (\w+)", line)
            if m:
                table = m.group(1)
                continue
            m = re.match(r"\s+(\w+)\s+(TEXT|BYTEA)\b", line)
            if m and m.group(1) in TARGETS and table:
                found.append((table, m.group(1), m.group(2), path.split("/")[-1]))

    for table, col, typ, f in found:
        print(f"{table}.{col}  {typ}  ({f})")
    print(f"\n{len(found)} column(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
