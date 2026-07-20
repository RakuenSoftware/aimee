#!/usr/bin/env python3
"""Validate Aimee's canonical required/optional module inventory."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path


MODULE_ID = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
DEFAULT_INVENTORY = Path("tests/baselines/modules/canonical-inventory.yaml")
ALLOWED_KEYS = {"schema_version", "required", "optional"}


class InventoryError(ValueError):
    """A closed, operator-readable inventory validation failure."""


def _line_for(path: Path, value: str) -> int | None:
    needle = json.dumps(value)
    try:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if needle in line:
                return number
    except OSError:
        return None
    return None


def _fail(rule: str, message: str, *, path: Path, value: str | None = None) -> None:
    location = str(path)
    if value is not None:
        line = _line_for(path, value)
        if line is not None:
            location += f":{line}"
    raise InventoryError(f"{location}: rule={rule}: {message}")


def _string_list(data: dict[str, object], key: str, path: Path) -> list[str]:
    value = data.get(key)
    if not isinstance(value, list):
        _fail("structure", f"expected {key} to be an array", path=path)
    if not all(isinstance(item, str) for item in value):
        _fail("structure", f"expected every {key} entry to be a string", path=path)
    return value


def load_inventory(path: Path) -> tuple[dict[str, object], list[str], list[str]]:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise InventoryError(f"{path}: rule=input: cannot read inventory: {exc.strerror}") from exc

    try:
        data = json.loads(raw)
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
    *,
    schema_version: int,
    required_count: int,
    optional_count: int,
    required_modules: set[str],
    forbidden_optional: set[str],
    forbidden_modules: set[str],
) -> None:
    data, required, optional = load_inventory(path)

    if data["schema_version"] != schema_version:
        _fail(
            "schema-version",
            f"expected {schema_version}, actual {data['schema_version']!r}",
            path=path,
        )

    all_ids = required + optional
    invalid = next((module_id for module_id in all_ids if not MODULE_ID.fullmatch(module_id)), None)
    if invalid is not None:
        _fail("module-id-syntax", f"invalid module ID {invalid!r}", path=path, value=invalid)

    for group, values in (("required", required), ("optional", optional)):
        duplicates = sorted({module_id for module_id in values if values.count(module_id) > 1})
        if duplicates:
            duplicate = duplicates[0]
            _fail("unique-ids", f"duplicate {group} module {duplicate!r}", path=path, value=duplicate)

    overlap = sorted(set(required) & set(optional))
    if overlap:
        module_id = overlap[0]
        _fail("disjoint-sets", f"module {module_id!r} is both required and optional", path=path, value=module_id)

    for module_id in sorted(required_modules):
        if module_id not in required:
            actual = "optional" if module_id in optional else "absent"
            _fail(
                "required-classification",
                f"module {module_id!r}: expected required, actual {actual}",
                path=path,
                value=module_id,
            )
    for module_id in sorted(forbidden_optional):
        if module_id in optional:
            _fail(
                "optional-classification",
                f"module {module_id!r}: expected not optional, actual optional",
                path=path,
                value=module_id,
            )
    for module_id in sorted(forbidden_modules):
        if module_id in all_ids:
            _fail("forbidden-module", f"forbidden module ID {module_id!r}", path=path, value=module_id)

    if len(required) != required_count:
        _fail(
            "required-count",
            f"expected {required_count}, actual {len(required)}",
            path=path,
        )
    if len(optional) != optional_count:
        _fail(
            "optional-count",
            f"expected {optional_count}, actual {len(optional)}",
            path=path,
        )


def _csv(values: list[str]) -> set[str]:
    return {item for value in values for item in value.split(",") if item}


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path)
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--schema-version", type=int, default=1)
    parser.add_argument("--required-count", type=int, default=18)
    parser.add_argument("--optional-count", type=int, default=8)
    parser.add_argument("--require-required-module", action="append", default=["git"])
    parser.add_argument("--forbid-optional-module", action="append", default=["git"])
    parser.add_argument("--forbid-module", action="append", default=[])
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    script_root = Path(__file__).resolve().parent.parent
    config_root = args.config_root or Path(os.environ.get("AIMEE_CONFIG_ROOT", script_root))
    config_root = config_root.resolve()
    inventory = args.inventory if args.inventory.is_absolute() else config_root / args.inventory

    try:
        validate_inventory(
            inventory,
            schema_version=args.schema_version,
            required_count=args.required_count,
            optional_count=args.optional_count,
            required_modules=_csv(args.require_required_module),
            forbidden_optional=_csv(args.forbid_optional_module),
            forbidden_modules=_csv(args.forbid_module),
        )
    except InventoryError as exc:
        print(f"check_module_inventory: error: {exc}", file=sys.stderr)
        return 1

    print(
        f"check_module_inventory: ok ({args.required_count} required, "
        f"{args.optional_count} optional; {inventory})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
