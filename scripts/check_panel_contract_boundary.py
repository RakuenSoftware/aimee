#!/usr/bin/env python3
"""Keep optional roundtable execution contracts out of required consumers."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROUND_TABLE_TYPES = "roundtable_types.h"
ENSEMBLE_HEADER = "delegate_ensemble.h"
# Exact migration debt on testing. The panel provider/roster API currently carries
# the panel value type from the optional owner, while these composition call sites
# still invoke the optional implementation directly. Keep this list closed until
# the provider ABI is linked into shipping binaries; no new consumer may join it.
TEMPORARY_TYPE_CONSUMERS = {
    "src/modules/delegates/include/aimee/delegates/panel_provider.h",
    # panel_roster.h left with panel_roster.c: neither reached a shipped binary
    # (`./aimee` has no such command path) and nothing live referenced them.
}
TEMPORARY_ENSEMBLE_CONSUMERS = {
    "src/cmd_agent_delegate.c",
    "src/headers/evidence_replay.h",
    "src/headers/server_compute_internal.h",
    # wfe_live_panel.c paid this debt off: its delegate_ensemble.h include was
    # dead (the compiler builds the TU without it), so it was deleted rather
    # than migrated. The list only ever shrinks.
    "src/modules/workflows/wfe_panel_roundtable.h",
    "src/server/server_compute.c",
    "src/server/server_compute_roundtable.c",
    "src/server/server_sweep.c",
}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


class CheckError(ValueError):
    """A panel contract crossed the optional-module boundary."""


def _includes(path: Path) -> set[str]:
    return {Path(value).name for value in INCLUDE.findall(path.read_text(encoding="utf-8"))}


def validate(root: Path) -> None:
    src = root / "src"
    if not src.is_dir():
        raise CheckError("rule=source-root-missing path=src")

    actual_type_consumers: set[str] = set()
    actual_ensemble_consumers: set[str] = set()
    for path in sorted((*src.rglob("*.c"), *src.rglob("*.h"))):
        relative = path.relative_to(root).as_posix()
        includes = _includes(path)
        owner_private = relative.startswith("src/modules/roundtable/")
        test_code = relative.startswith("src/tests/")

        if ROUND_TABLE_TYPES in includes and not (owner_private or test_code):
            if relative not in TEMPORARY_TYPE_CONSUMERS:
                raise CheckError(f"rule=optional-type-header-leak path={relative}")
            actual_type_consumers.add(relative)
        if ENSEMBLE_HEADER in includes and not (owner_private or test_code):
            if relative not in TEMPORARY_ENSEMBLE_CONSUMERS:
                raise CheckError(f"rule=optional-ensemble-header-leak path={relative}")
            actual_ensemble_consumers.add(relative)

    for label, actual, expected in (
        ("type", actual_type_consumers, TEMPORARY_TYPE_CONSUMERS),
        ("ensemble", actual_ensemble_consumers, TEMPORARY_ENSEMBLE_CONSUMERS),
    ):
        if actual != expected:
            missing = sorted(expected - actual)
            unexpected = sorted(actual - expected)
            raise CheckError(
                f"rule=temporary-{label}-header-debt missing={missing} unexpected={unexpected}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
    except (CheckError, OSError, UnicodeError) as exc:
        print(f"panel-contract-boundary: ERROR {exc}", file=sys.stderr)
        return 1
    print("panel-contract-boundary: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
