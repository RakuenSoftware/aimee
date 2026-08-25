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
ALLOWED_KEYS = {
    "schema_version",
    "required",
    "optional",
    "principal_refs",
    "retired_principal_refs",
    "plugin_principal_ref_band",
    "db3_provider_principal_ref_band",
}

# Every band of principal refs reserved for DYNAMICALLY provisioned processes.
# A ref carves a whole 256-kind block (4096 + ref*256 + stage), so a module
# assigned a ref inside one of these would have its kinds land where a
# provisioned instance may already be serving.
#
# Generalised to a list rather than special-casing the plugin band: DB3 vector
# providers need exactly the same protection, and a second hand-written copy of
# this rule is how the two would drift.
RESERVED_BANDS = (
    ("plugin_principal_ref_band", "plugin instances"),
    ("db3_provider_principal_ref_band", "DB3 vector providers"),
)
REQUIRED_COUNT = 19
# PostgreSQL is appended while its C-to-Go migration is staged so existing
# optional-module principal references remain stable.
#
# vectordb follows it with no principal_refs entry of its own. It is a DB3
# vector provider, and a provider is dynamically provisioned -- its ref comes
# from db3_provider_principal_ref_band at provision time, not from this file.
# It is listed here anyway because a module id is what a descriptor is validated
# against, and without one server-go/modules/vectordb has no id, no dependency
# edges, and nothing for check-module-descriptor-sources to claim its files by.
OPTIONAL_COUNT = 12
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

    if type(data["schema_version"]) is not int or data["schema_version"] != 2:
        _fail(
            "schema-version",
            f"expected 2, actual {data['schema_version']!r}",
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

    _validate_principal_refs(data, path)


def _validate_principal_refs(data: dict[str, object], path: Path) -> None:
    """Refs are unique, and none falls inside the band reserved for plugins.

    Event kinds are carved from a ref as 4096 + ref*256 + stage, so a ref is not
    just an id: it reserves a whole 256-kind block. Handing a module a ref inside
    the plugin band would put its kinds where a provisioned plugin instance may
    already be serving, and bus_host_serve_kind() binds one kind to exactly one
    slot -- the loser is denied at attach with nothing in its own log to say why.
    That is not hypothetical: the plugin range formerly sat at 11264, which is
    postgres's block (4096 + 28*256), and a live aimee-kb reproduced the denial.
    """
    refs = data.get("principal_refs")
    if not isinstance(refs, dict):
        _fail("structure", "expected principal_refs to be an object", path=path)

    bands = []
    for key, what in RESERVED_BANDS:
        band = data.get(key)
        if (
            not isinstance(band, dict)
            or type(band.get("first")) is not int
            or type(band.get("limit")) is not int
        ):
            _fail(
                "structure",
                f"expected {key} to be an object with integer first/limit",
                path=path,
            )
        first, limit = band["first"], band["limit"]
        if first >= limit:
            _fail("plugin-band", f"{key}: expected first < limit, actual {first} >= {limit}",
                  path=path)
        bands.append((key, what, first, limit))

    # Reserved bands must not overlap each other either -- two dynamic allocators
    # drawing from one range is the same defect as a module inside a band.
    for i, (k1, _, f1, l1) in enumerate(bands):
        for k2, _, f2, l2 in bands[i + 1:]:
            if f1 < l2 and f2 < l1:
                _fail("plugin-band",
                      f"{k1} [{f1},{l1}) overlaps {k2} [{f2},{l2})", path=path)

    seen: dict[int, str] = {}
    for module_id, ref in sorted(refs.items()):
        if type(ref) is not int or ref < 1:
            _fail("principal-ref", f"module {module_id!r}: expected a positive integer ref, "
                  f"actual {ref!r}", path=path)
        if ref in seen:
            _fail("principal-ref", f"modules {seen[ref]!r} and {module_id!r} share "
                  f"principal_ref={ref}", path=path)
        seen[ref] = module_id
        for key, what, first, limit in bands:
            if first <= ref < limit:
                _fail(
                    "plugin-band",
                    f"module {module_id!r} has principal_ref={ref}, inside {key} "
                    f"[{first},{limit}) reserved for {what}; its event kinds "
                    f"({4096 + ref * 256}+) would collide with a provisioned instance",
                    path=path,
                )

    retired = data.get("retired_principal_refs")
    if not isinstance(retired, list) or not all(type(r) is int for r in retired):
        _fail("structure", "expected retired_principal_refs to be an array of integers",
              path=path)
    for ref in retired:
        if ref in seen:
            _fail("principal-ref", f"principal_ref={ref} is retired but still assigned to "
                  f"{seen[ref]!r}; a retired ref is never recycled", path=path)


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
