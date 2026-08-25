#!/usr/bin/env python3
"""Fail closed unless every pgvector declaration has a reviewed DB3 disposition.

The audit is metadata, not a numeric data format: JSON integers are allowed but
floating-point tokens are deliberately rejected along with NaN and Infinity.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import NoReturn


ROOT = Path(__file__).resolve().parent.parent
AUDIT = Path("src/modules/db2/eventcontract/vector-portability.json")
LEDGER = Path("tests/baselines/db2/declarations-v1.json")
MAX_BYTES = 2_097_152
MAX_DEPTH = 32
MAX_ARRAY = 4096
SYMBOL = re.compile(r"^pgvec_[a-z0-9_]+$")
CLASSIFICATIONS = (
    ("portable-search", "portable-now", "candidate-search"),
    ("committed-mutation", "portable-after-commit", "apply"),
    ("provider-control", "provider-local", "provider-control"),
    ("db2-authority", "retained-db2", "none"),
    ("portable-analytics", "deferred", "candidate-analytics"),
)


class PortabilityError(ValueError):
    """A typed portability-audit failure."""


def fail(rule: str, message: str) -> NoReturn:
    raise PortabilityError(f"rule={rule}: {message}")


def _duplicates(label: str):
    def reject(pairs: list[tuple[str, object]]) -> dict[str, object]:
        value: dict[str, object] = {}
        for key, item in pairs:
            if key in value:
                fail("json-duplicate-key", f"{label}: duplicate key {key!r}")
            value[key] = item
        return value
    return reject


def _reject_number(value: str) -> NoReturn:
    fail("json-number-domain", f"forbidden number {value!r}")


def _domain(value: object, label: str, depth: int = 0) -> None:
    if depth > MAX_DEPTH:
        fail("json-depth", f"{label}: nesting exceeds {MAX_DEPTH}")
    if isinstance(value, str):
        if any(0xD800 <= ord(char) <= 0xDFFF for char in value):
            fail("json-surrogate", f"{label}: surrogate code point is forbidden")
    elif isinstance(value, list):
        if len(value) > MAX_ARRAY:
            fail("json-array-size", f"{label}: array exceeds {MAX_ARRAY} items")
        for item in value:
            _domain(item, label, depth + 1)
    elif isinstance(value, dict):
        for key, item in value.items():
            _domain(key, label, depth + 1)
            _domain(item, label, depth + 1)


def load_json(path: Path) -> object:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("input", f"cannot read {path}: {exc}")
    if len(raw) > MAX_BYTES:
        fail("input-size", f"{path} exceeds {MAX_BYTES} bytes")
    if raw.startswith(b"\xef\xbb\xbf"):
        fail("json-bom", f"{path} begins with a UTF-8 BOM")
    try:
        value = json.loads(
            raw.decode("utf-8", "strict"),
            object_pairs_hook=_duplicates(str(path)),
            parse_float=_reject_number,
            parse_constant=_reject_number,
        )
    except UnicodeDecodeError as exc:
        fail("json-encoding", f"{path}: {exc}")
    except json.JSONDecodeError as exc:
        fail("json-parse", f"{path}: {exc.msg} at {exc.lineno}:{exc.colno}")
    _domain(value, str(path))
    return value


def _object(value: object, keys: set[str], label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        fail("shape", f"{label} must be an object")
    if set(value) != keys:
        fail("keys", f"{label} keys differ; expected={sorted(keys)}, actual={sorted(value)}")
    return value


def _text(value: object, label: str, maximum: int) -> str:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > maximum:
        fail("string", f"{label} must be nonempty and at most {maximum} UTF-8 bytes")
    return value


def source_symbols(ledger: object) -> list[str]:
    if not isinstance(ledger, dict) or not isinstance(ledger.get("declarations"), list):
        fail("ledger-shape", "declaration ledger must contain a declarations array")
    symbols: list[str] = []
    for index, declaration in enumerate(ledger["declarations"]):
        if not isinstance(declaration, dict) or not isinstance(declaration.get("symbol"), str):
            fail("ledger-declaration", f"declarations[{index}] has no string symbol")
        symbol = declaration["symbol"]
        if symbol.startswith("pgvec_"):
            if not SYMBOL.fullmatch(symbol):
                fail("ledger-symbol", f"invalid pgvector symbol {symbol!r}")
            symbols.append(symbol)
    if len(symbols) != len(set(symbols)):
        fail("ledger-duplicate", "pgvector declaration symbols are not unique")
    return sorted(symbols)


def symbols_sha256(symbols: list[str]) -> str:
    return hashlib.sha256("".join(f"{symbol}\n" for symbol in symbols).encode()).hexdigest()


def validate(audit_value: object, ledger_value: object) -> dict[str, int]:
    audit = _object(audit_value, {
        "schema_version", "module", "source", "source_symbols_sha256", "classifications",
    }, "audit")
    if type(audit["schema_version"]) is not int or audit["schema_version"] != 1:
        fail("schema-version", "schema_version must equal 1")
    if audit["module"] != "db3":
        fail("module", "module must equal 'db3'")
    if audit["source"] != LEDGER.as_posix():
        fail("source", f"source must equal {LEDGER.as_posix()!r}")
    source = source_symbols(ledger_value)
    expected_hash = symbols_sha256(source)
    if audit["source_symbols_sha256"] != expected_hash:
        fail("source-fingerprint", "source_symbols_sha256 does not match the pgvector declarations")

    groups = audit["classifications"]
    if not isinstance(groups, list) or len(groups) != len(CLASSIFICATIONS):
        fail("classifications", f"classifications must contain {len(CLASSIFICATIONS)} entries")
    seen: set[str] = set()
    summary: dict[str, int] = {}
    for index, (raw, expected) in enumerate(zip(groups, CLASSIFICATIONS, strict=True)):
        group = _object(raw, {
            "id", "disposition", "db3_operation_family", "rationale", "symbols",
        }, f"classifications[{index}]")
        identifier, disposition, family = expected
        if (group["id"], group["disposition"], group["db3_operation_family"]) != expected:
            fail("classification-identity", f"classification {index} must equal {expected!r}")
        _text(group["rationale"], f"{identifier}.rationale", 512)
        symbols = group["symbols"]
        if not isinstance(symbols, list) or not symbols:
            fail("classification-symbols", f"{identifier}.symbols must be a nonempty array")
        if symbols != sorted(symbols):
            fail("symbol-order", f"{identifier}.symbols must be sorted")
        for symbol in symbols:
            if not isinstance(symbol, str) or not SYMBOL.fullmatch(symbol):
                fail("symbol", f"{identifier} contains invalid symbol {symbol!r}")
            if symbol in seen:
                fail("symbol-duplicate", f"{symbol} is classified more than once")
            seen.add(symbol)
        summary[identifier] = len(symbols)

    missing = sorted(set(source) - seen)
    extra = sorted(seen - set(source))
    if missing or extra:
        fail("coverage", f"missing={missing}, extra={extra}")
    return summary


def run(root: Path) -> dict[str, int]:
    summary = validate(load_json(root / AUDIT), load_json(root / LEDGER))
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--print-source-fingerprint", action="store_true")
    args = parser.parse_args()
    try:
        if args.print_source_fingerprint:
            symbols = source_symbols(load_json(args.root.resolve() / LEDGER))
            print(f"{symbols_sha256(symbols)}  pgvec-symbols={len(symbols)}")
            return 0
        summary = run(args.root.resolve())
    except PortabilityError as exc:
        print(f"check_db3_portability: error: {exc}", file=sys.stderr)
        return 1
    rendered = ", ".join(f"{name}={count}" for name, count in summary.items())
    print(f"check_db3_portability: ok (total={sum(summary.values())}; {rendered})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
