#!/usr/bin/env python3
"""Guard that every command the thin client can route has a help entry.

`aimee help <cmd>` is served from client_help[] (src/cli_help_data.h), which is a
SEPARATE table from both the /v1 route map (src/cli_v1_routes.c) and the embedded
command table (src/cmd_table.c). Nothing tied them together, so a command could
route and execute perfectly while `aimee help <cmd>` answered "Unknown command:
<cmd>" and `aimee help --all` never listed it. That is exactly what happened to
`aux`: fully routed, fully working, invisible to help.

This check fails `make lint` when a routed command has no client_help[] entry, so
the next command to ship cannot repeat it.

KNOWN_GAPS is empty and should stay that way: the seven original violations
(audit, ensemble, migrate, notes, pipeline, repo, workers) were filled in from
authoritative sources rather than guessed. Adding a name here is a regression —
write the help entry instead.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROUTES = ROOT / "src" / "cli_v1_routes.c"
HELP = ROOT / "src" / "cli_help_data.h"

# Routed commands that still lack a client_help[] entry. Shrink this, never grow it.
KNOWN_GAPS: set[str] = set()


def main() -> int:
    routed = set(re.findall(r'^\s*\{"([a-z0-9-]+)",\s*"', ROUTES.read_text(encoding="utf-8"), re.M))
    helped = set(re.findall(r'\{"([a-z0-9-]+)",\s*"', HELP.read_text(encoding="utf-8")))
    if not routed or not helped:
        print("check-cli-help-coverage: FAIL — could not parse the route or help table; "
              "the patterns in this script have drifted from the source.")
        return 1

    missing = (routed - helped) - KNOWN_GAPS
    if missing:
        print("check-cli-help-coverage: FAIL — routed with no client_help[] entry, so "
              "`aimee help <cmd>` answers \"Unknown command\":")
        for name in sorted(missing):
            print(f"  {name}")
        print("Add an entry to src/cli_help_data.h (see the `aux` entry for the shape).")
        return 1

    # A fixed gap must leave the list, or the list rots into a lie.
    stale = KNOWN_GAPS & helped
    if stale:
        print("check-cli-help-coverage: FAIL — these have help entries now but are still "
              "listed as KNOWN_GAPS; remove them from the list in this script:")
        for name in sorted(stale):
            print(f"  {name}")
        return 1

    covered = len(routed & helped)
    print(f"check-cli-help-coverage: ok ({covered}/{len(routed)} routed commands documented, "
          f"{len(KNOWN_GAPS)} known gaps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
