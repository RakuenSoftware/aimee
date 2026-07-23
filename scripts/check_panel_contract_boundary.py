#!/usr/bin/env python3
"""Keep required panel-result consumers independent of optional roundtable headers."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROUND_TABLE_TYPES = "roundtable_types.h"
ENSEMBLE_HEADER = "delegate_ensemble.h"
ENSEMBLE_CONSUMERS = {
    "src/cmd_agent_delegate.c",
    "src/modules/workflows/wfe_live_panel.c",
    "src/server/server_compute.c",
    "src/server/server_sweep.c",
}
MIGRATED_CONSUMERS = {
    "src/cmd_agent_delegate_toolset.c",
    "src/headers/evidence_replay.h",
    "src/headers/server_compute_internal.h",
    "src/modules/workflows/wfe_panel_roundtable.c",
    "src/modules/workflows/wfe_panel_roundtable.h",
    "src/server/server_compute_mailbox.c",
    "src/server/server_compute_roundtable.c",
    "src/server/server_pipeline.c",
    "src/server/server_pipeline_merge.c",
    "src/server_compute_episodes.c",
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

    actual_ensemble_consumers: set[str] = set()
    for path in sorted((*src.rglob("*.c"), *src.rglob("*.h"))):
        relative = path.relative_to(root).as_posix()
        includes = _includes(path)
        owner_private = relative.startswith("src/modules/roundtable/")
        test_code = relative.startswith("src/tests/")

        if ROUND_TABLE_TYPES in includes and not (owner_private or test_code):
            raise CheckError(f"rule=optional-type-header-leak path={relative}")
        if ENSEMBLE_HEADER in includes and not (owner_private or test_code):
            actual_ensemble_consumers.add(relative)

    if actual_ensemble_consumers != ENSEMBLE_CONSUMERS:
        missing = sorted(ENSEMBLE_CONSUMERS - actual_ensemble_consumers)
        unexpected = sorted(actual_ensemble_consumers - ENSEMBLE_CONSUMERS)
        raise CheckError(
            f"rule=ensemble-consumer-allowlist missing={missing} unexpected={unexpected}"
        )

    for relative in sorted(MIGRATED_CONSUMERS):
        path = root / relative
        if not path.is_file():
            raise CheckError(f"rule=migrated-consumer-missing path={relative}")
        includes = _includes(path)
        if ROUND_TABLE_TYPES in includes or ENSEMBLE_HEADER in includes:
            raise CheckError(f"rule=migrated-consumer-regression path={relative}")


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
