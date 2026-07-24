#!/usr/bin/env python3
"""Validate the modular-refactor cleanup ledger."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LEDGER = Path("tests/baselines/refactor/cleanup-ledger.json")
STATES = {"present_and_verified", "absent_with_reason", "present_and_unverified"}
ENTRY_KEYS = {
    "slice",
    "state",
    "disposition",
    "production",
    "fallbacks",
    "net_growth",
    "consumers",
    "blast_radius",
    "independent_review",
    "evidence",
}
PRODUCTION_KEYS = {"added", "deleted", "consolidated"}
GROWTH_KEYS = {"status", "rationale", "rejected_simpler_alternative", "revisit"}


class LedgerError(ValueError):
    """A closed, operator-readable ledger validation failure."""


def _object_without_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise LedgerError(f"duplicate object key {key!r}")
        result[key] = value
    return result


def _tracked_files(root: Path) -> set[str]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"], check=True, capture_output=True
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise LedgerError(f"cannot enumerate tracked repository files: {exc}") from exc
    return {item.decode("utf-8") for item in result.stdout.split(b"\0") if item}


def _nonempty_string(value: object, field: str) -> None:
    if not isinstance(value, str) or not value.strip():
        raise LedgerError(f"{field} must be a non-empty string")


def _string_list(value: object, field: str, *, allow_empty: bool = True) -> None:
    if not isinstance(value, list) or not all(isinstance(item, str) and item.strip() for item in value):
        raise LedgerError(f"{field} must be an array of non-empty strings")
    if not allow_empty and not value:
        raise LedgerError(f"{field} must not be empty")


def _exact_keys(value: object, expected: set[str], field: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise LedgerError(f"{field} must be an object")
    actual = set(value)
    if actual != expected:
        raise LedgerError(
            f"{field} keys differ: missing={sorted(expected - actual)}, unknown={sorted(actual - expected)}"
        )
    return value


def validate(path: Path, root: Path, *, allow_unverified: bool = False) -> None:
    try:
        data = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_object_without_duplicate_keys
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LedgerError(f"cannot load {path}: {exc}") from exc
    top = _exact_keys(data, {"schema_version", "entries"}, "ledger")
    if top["schema_version"] != 1 or type(top["schema_version"]) is not int:
        raise LedgerError("schema_version must be integer 1")
    entries = top["entries"]
    if not isinstance(entries, list) or not entries:
        raise LedgerError("entries must be a non-empty array")
    seen: set[int] = set()
    previous = -1
    tracked = _tracked_files(root)
    for index, raw in enumerate(entries):
        prefix = f"entries[{index}]"
        entry = _exact_keys(raw, ENTRY_KEYS, prefix)
        slice_no = entry["slice"]
        if type(slice_no) is not int or slice_no <= 0:
            raise LedgerError(f"{prefix}.slice must be a positive integer")
        if slice_no in seen:
            raise LedgerError(f"duplicate slice entry: {slice_no}")
        if slice_no <= previous:
            raise LedgerError("entries must be sorted by ascending slice")
        seen.add(slice_no)
        previous = slice_no
        state = entry["state"]
        if not isinstance(state, str):
            raise LedgerError(f"{prefix}.state must be a string")
        if state not in STATES:
            raise LedgerError(f"{prefix}.state must be one of {sorted(STATES)}")
        if state == "present_and_unverified" and not allow_unverified:
            raise LedgerError(f"slice {slice_no} is present_and_unverified")
        _nonempty_string(entry["disposition"], f"{prefix}.disposition")
        production = _exact_keys(entry["production"], PRODUCTION_KEYS, f"{prefix}.production")
        for key in PRODUCTION_KEYS:
            _string_list(production[key], f"{prefix}.production.{key}")
        _string_list(entry["fallbacks"], f"{prefix}.fallbacks")
        growth = _exact_keys(entry["net_growth"], GROWTH_KEYS, f"{prefix}.net_growth")
        growth_status = growth["status"]
        if not isinstance(growth_status, str):
            raise LedgerError(f"{prefix}.net_growth.status must be a string")
        if growth_status not in {"none", "justified"}:
            raise LedgerError(f"{prefix}.net_growth.status must be none or justified")
        for key in GROWTH_KEYS - {"status"}:
            if growth_status == "justified":
                _nonempty_string(growth[key], f"{prefix}.net_growth.{key}")
            elif not isinstance(growth[key], str):
                raise LedgerError(f"{prefix}.net_growth.{key} must be a string")
        _string_list(
            entry["consumers"],
            f"{prefix}.consumers",
            allow_empty=state == "absent_with_reason",
        )
        _nonempty_string(entry["blast_radius"], f"{prefix}.blast_radius")
        _nonempty_string(entry["independent_review"], f"{prefix}.independent_review")
        _string_list(
            entry["evidence"],
            f"{prefix}.evidence",
            allow_empty=state == "absent_with_reason",
        )
        for evidence in entry["evidence"]:
            evidence_path = evidence.split("#", 1)[0]
            resolved = Path(os.path.realpath(root / evidence_path))
            try:
                resolved.relative_to(root)
            except ValueError as exc:
                raise LedgerError(f"slice {slice_no} evidence escapes repository: {evidence}") from exc
            if not resolved.is_file():
                raise LedgerError(f"slice {slice_no} evidence does not exist: {evidence}")
            relative = resolved.relative_to(root).as_posix()
            if relative not in tracked:
                raise LedgerError(f"slice {slice_no} evidence is not Git-tracked: {evidence}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    parser.add_argument("--config-root", type=Path, default=ROOT)
    parser.add_argument("--allow-unverified", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    root = Path(os.path.realpath(args.config_root))
    ledger = args.ledger if args.ledger.is_absolute() else root / args.ledger
    ledger = Path(os.path.realpath(ledger))
    try:
        if not root.is_dir():
            raise LedgerError(f"config root is not a directory: {root}")
        ledger.relative_to(root)
        validate(ledger, root, allow_unverified=args.allow_unverified)
    except (LedgerError, ValueError) as exc:
        print(f"check_cleanup_ledger: error: {exc}", file=sys.stderr)
        return 1
    print(f"check_cleanup_ledger: ok ({ledger.relative_to(root)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
