#!/usr/bin/env python3
"""Which of the db1 client's lifecycle entry points are still called.

src/db1_client/db1.h declares a SQLite-era lifecycle -- open a file at a path,
apply pragmas, close it -- alongside the bus-era readiness calls that replaced
it. The header is the public umbrella every caller includes, so a declaration
that no longer has an implementation still reads as API someone must use.
"""

import re
import subprocess
import sys

NAMES = [
    "db1_init",
    "db1_shutdown",
    "db1_is_initialized",
    "db1_apply_server_pragmas",
    "db1_store_ready",
    "db1_store_probe",
]

# The declaration itself, and the test mock that exists only so callers link.
IGNORE = ("src/db1_client/db1.h", "src/tests/support/db1_init_mock.c")


def callers(name: str) -> list[str]:
    out = subprocess.run(
        ["grep", "-rn", "--include=*.c", "--include=*.h", f"{name}(", "src/"],
        capture_output=True, text=True,
    ).stdout.splitlines()
    hits = []
    for line in out:
        path = line.split(":", 1)[0]
        if path.endswith(IGNORE) or any(path == i for i in IGNORE):
            continue
        # A declaration in some other header is not a call either.
        body = line.split(":", 2)[-1].strip()
        if re.match(r"^(extern\s+)?(int|void)\s+" + name + r"\s*\(", body):
            continue
        hits.append(line)
    return hits


def main() -> int:
    for name in NAMES:
        hits = callers(name)
        print(f"{name:28s} {len(hits):3d} caller(s)")
        for h in hits[:4]:
            print(f"      {h[:110]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
