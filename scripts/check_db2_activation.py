#!/usr/bin/env python3
"""Refuse DB2 process activation before the C cutover is atomic."""

from __future__ import annotations

import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parent.parent
DESCRIPTOR = Path("src/modules/db2/module.yaml")
SOURCE_BASELINE = Path("tests/baselines/db2/source-boundary-v2.json")
DECLARATION_LEDGER = Path("tests/baselines/db2/declarations-v1.json")
ADAPTER = Path("src/modules/db2/module_adapter.c")


class ActivationError(ValueError):
    """A DB2 activation invariant is not satisfied."""


def fail(rule: str, message: str) -> None:
    raise ActivationError(f"rule={rule}: {message}")


def load(root: Path, relative: Path) -> object:
    try:
        raw = (root / relative).read_bytes()
        if raw.startswith(b"\xef\xbb\xbf"):
            fail("json-bom", f"{relative} begins with a UTF-8 BOM")
        return json.loads(raw.decode("utf-8", "strict"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail("input", f"cannot load {relative}: {exc}")


def check(root: Path) -> None:
    descriptor = load(root, DESCRIPTOR)
    if not isinstance(descriptor, dict) or type(descriptor.get("enabled_by_default")) is not bool:
        fail("descriptor", "DB2 descriptor has no boolean enabled_by_default")
    if not descriptor["enabled_by_default"]:
        return

    sources = descriptor.get("sources")
    if not isinstance(sources, list) or not all(isinstance(row, str) for row in sources):
        fail("source-closure", "DB2 descriptor sources are not a string array")
    baseline = load(root, SOURCE_BASELINE)
    if (not isinstance(baseline, dict) or not isinstance(baseline.get("source_files"), dict) or
            not isinstance(baseline["source_files"].get("c"), list)):
        fail("source-closure", "DB2 source baseline has no C source inventory")
    missing = sorted(set(baseline["source_files"]["c"]) - set(sources))
    if missing:
        fail("source-closure",
             f"enabled DB2 process omits {len(missing)} C sources; first={missing[0]}")

    try:
        adapter = (root / ADAPTER).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        fail("adapter", f"cannot read {ADAPTER}: {exc}")
    if "__attribute__((weak))" in adapter or "#pragma weak" in adapter:
        fail("weak-backend", "enabled DB2 process still permits an absent backend")

    ledger = load(root, DECLARATION_LEDGER)
    if (not isinstance(ledger, dict) or not isinstance(ledger.get("declarations"), list) or
            not isinstance(ledger.get("consumer_classes"), list)):
        fail("direct-caller", "DB2 declaration ledger has invalid shape")
    classes = {
        row.get("path"): row.get("classification")
        for row in ledger["consumer_classes"] if isinstance(row, dict)
    }
    wire_operations = [
        row for row in ledger["declarations"]
        if isinstance(row, dict)
        and isinstance(row.get("review"), dict)
        and row["review"].get("disposition") == "wire-operation"
    ]
    if not wire_operations:
        fail("direct-caller", "DB2 declaration ledger has no reviewed wire operations")
    for operation in wire_operations:
        symbol = operation.get("symbol")
        consumers = operation.get("consumers")
        if not isinstance(symbol, str) or not isinstance(consumers, list):
            fail("direct-caller", "reviewed wire operation has an invalid ledger entry")
        production = sorted(path for path in consumers
                            if classes.get(path) != "private-implementation-test")
        if production:
            fail("direct-caller",
                 f"enabled DB2 wire operation {symbol} still has direct production callers: "
                 f"{production}")


def main() -> int:
    try:
        check(ROOT)
    except ActivationError as exc:
        print(f"check_db2_activation: error: {exc}", file=sys.stderr)
        return 1
    print("check_db2_activation: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
