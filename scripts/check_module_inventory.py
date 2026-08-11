#!/usr/bin/env python3
"""Validate Aimee's canonical required/optional module inventory.

The ``.yaml`` contract is restricted to JSON-compatible YAML and parsed only
with the standard-library JSON decoder. Relative inventory paths resolve against
``--config-root``, then ``AIMEE_CONFIG_ROOT``, then the repository root derived
from this script, in that order.
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
import os
import re
import sys
from pathlib import Path


MODULE_ID = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
DEFAULT_INVENTORY = Path("tests/baselines/modules/canonical-inventory.yaml")
ALLOWED_KEYS = {"schema_version", "required", "optional"}
REQUIRED_COUNT = 18
# 8, not 7: dropping plugin-loader took this 8 -> 7 (e545e14dbc), then migrating
# the learned sandbox toolchain to a Go module added `sandbox` back (1b0187e3de)
# without updating the constant. The guard has been failing on testing ever since
# — and because it is not wired into `make lint`, nothing surfaced it.
OPTIONAL_COUNT = 9
PINNED_REQUIRED = {"git"}


class InventoryError(ValueError):
    """A closed, operator-readable inventory validation failure."""


def _fail(rule: str, message: str, *, path: Path) -> None:
    raise InventoryError(f"{path}: rule={rule}: {message}")


def _string_list(data: dict[str, object], key: str, path: Path) -> list[str]:
    value = data.get(key)
    if not isinstance(value, list):
        _fail(
            "structure",
            f"expected {key} to be an array, actual {type(value).__name__}",
            path=path,
        )
    elif not all(isinstance(item, str) for item in value):
        _fail("structure", f"expected every {key} entry to be a string", path=path)
    return value


def _object_without_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise InventoryError(f"rule=structure: duplicate object key {key!r}")
        result[key] = value
    return result


def load_inventory(path: Path) -> tuple[dict[str, object], list[str], list[str]]:
    try:
        raw = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise InventoryError(f"{path}: rule=input: cannot read inventory: {exc}") from exc

    try:
        data = json.loads(raw, object_pairs_hook=_object_without_duplicate_keys)
    except InventoryError as exc:
        raise InventoryError(f"{path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise InventoryError(
            f"{path}:{exc.lineno}:{exc.colno}: rule=parse: inventory must be safe JSON-compatible YAML: {exc.msg}"
        ) from exc

    if not isinstance(data, dict):
        _fail("structure", "expected a top-level object", path=path)
    unknown = sorted(set(data) - ALLOWED_KEYS)
    missing = sorted(ALLOWED_KEYS - set(data))
    if unknown:
        _fail("structure", f"unknown keys: {', '.join(unknown)}", path=path)
    if missing:
        _fail("structure", f"missing keys: {', '.join(missing)}", path=path)

    required = _string_list(data, "required", path)
    optional = _string_list(data, "optional", path)
    return data, required, optional


def validate_inventory(
    path: Path,
) -> None:
    data, required, optional = load_inventory(path)

    if type(data["schema_version"]) is not int or data["schema_version"] != 1:
        _fail(
            "schema-version",
            f"expected 1, actual {data['schema_version']!r}",
            path=path,
        )

    all_ids = required + optional
    for group, values in (("required", required), ("optional", optional)):
        invalid = next((module_id for module_id in values if not MODULE_ID.fullmatch(module_id)), None)
        if invalid is not None:
            _fail(
                "module-id-syntax",
                f"invalid module ID {invalid!r}",
                path=path,
            )
        counts = Counter(values)
        duplicate = next((module_id for module_id in values if counts[module_id] > 1), None)
        if duplicate is not None:
            _fail(
                "unique-ids",
                f"duplicate {group} module {duplicate!r}",
                path=path,
            )

    overlap = sorted(set(required) & set(optional))
    if overlap:
        module_id = overlap[0]
        _fail(
            "disjoint-sets",
            f"module {module_id!r} is both required and optional",
            path=path,
        )

    for module_id in PINNED_REQUIRED:
        if module_id not in required:
            actual = "optional" if module_id in optional else "absent"
            _fail(
                "required-classification",
                f"module {module_id!r}: expected required, actual {actual}",
                path=path,
            )

    if len(required) != REQUIRED_COUNT:
        _fail(
            "required-count",
            f"expected REQUIRED_COUNT={REQUIRED_COUNT}, actual {len(required)}",
            path=path,
        )
    if len(optional) != OPTIONAL_COUNT:
        _fail(
            "optional-count",
            f"expected OPTIONAL_COUNT={OPTIONAL_COUNT}, actual {len(optional)}",
            path=path,
        )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path)
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    return parser.parse_args(argv)


def resolve_inventory(args: argparse.Namespace) -> Path:
    script_root = Path(__file__).resolve().parent.parent
    root_value = args.config_root or os.environ.get("AIMEE_CONFIG_ROOT") or script_root
    config_root = Path(os.path.realpath(root_value))
    if not config_root.is_dir():
        raise InventoryError(
            f"{config_root}: rule=config-root: expected an existing directory"
        )

    candidate = args.inventory if args.inventory.is_absolute() else config_root / args.inventory
    inventory = Path(os.path.realpath(candidate))
    try:
        inventory.relative_to(config_root)
    except ValueError as exc:
        raise InventoryError(
            f"{inventory}: rule=path: inventory must remain under config root {config_root}"
        ) from exc
    return inventory


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)

    try:
        inventory = resolve_inventory(args)
        validate_inventory(inventory)
    except InventoryError as exc:
        print(f"check_module_inventory: error: {exc}", file=sys.stderr)
        return 1

    print(
        f"check_module_inventory: ok ({REQUIRED_COUNT} required, "
        f"{OPTIONAL_COUNT} optional; {inventory})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
