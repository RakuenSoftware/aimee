#!/usr/bin/env python3
"""Ratchet every network-capable boundary that remains in the trusted C plane."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "src/modules/core-network-contracts.json"
SOURCE = ROOT / "src"
EXCLUDED_PREFIXES = ("src/tests/", "src/vendor/", "src/build/")

PRIMITIVES = {
    "credentialed_git": re.compile(
        r"\bgit_cred_inject_(?:build_env_for_repo|resolve_token)\s*\("
    ),
    "dns": re.compile(r"\bgetaddrinfo\s*\("),
    "http": re.compile(
        r"\bagent_http_(?:get(?:_location|_pinned|_stream)?|"
        r"post(?:_bytes|_content_type|_form)?|put|patch)\s*\("
    ),
    "inet_socket": re.compile(r"\bsocket\s*\(\s*(?:AF_INET|AF_INET6)"),
    "postgres": re.compile(r"\bPQconnect(?:db|Start)\s*\("),
    "tls_connect": re.compile(r"\bSSL_connect\s*\("),
}

REQUIRED_FIELDS = {
    "id",
    "owner",
    "direction",
    "purpose",
    "destination_constraint",
    "credential_constraint",
    "audit_disposition",
    "review_boundary",
    "matches",
}
ALLOWED_DIRECTIONS = {"inbound", "local", "outbound", "transport"}


def scan() -> set[tuple[str, str, int]]:
    found: set[tuple[str, str, int]] = set()
    for path in sorted(SOURCE.rglob("*.c")):
        relative = path.relative_to(ROOT).as_posix()
        if relative.startswith(EXCLUDED_PREFIXES):
            continue
        source = path.read_text(encoding="utf-8", errors="replace")
        for primitive, pattern in PRIMITIVES.items():
            count = len(pattern.findall(source))
            if count:
                found.add((primitive, relative, count))
    return found


def load_contract() -> tuple[list[dict[str, object]], list[str]]:
    failures: list[str] = []
    try:
        value = json.loads(CONTRACT.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"cannot load {CONTRACT.relative_to(ROOT)}: {exc}"]
    if not isinstance(value, dict) or set(value) != {"schema_version", "capabilities"}:
        return [], ["contract top level must contain only schema_version and capabilities"]
    if value["schema_version"] != 1 or not isinstance(value["capabilities"], list):
        return [], ["contract schema_version must be 1 and capabilities must be an array"]
    return value["capabilities"], failures


def expected_contract() -> tuple[set[tuple[str, str, int]], list[str]]:
    capabilities, failures = load_contract()
    expected: set[tuple[str, str, int]] = set()
    ids: set[str] = set()
    for index, capability in enumerate(capabilities, start=1):
        label = f"capability {index}"
        if not isinstance(capability, dict) or set(capability) != REQUIRED_FIELDS:
            failures.append(f"{label}: fields differ from the v1 contract")
            continue
        identifier = capability["id"]
        if not isinstance(identifier, str) or not identifier or identifier in ids:
            failures.append(f"{label}: id must be a unique non-empty string")
        else:
            ids.add(identifier)
            label = identifier
        for field in REQUIRED_FIELDS - {"id", "direction", "matches"}:
            if not isinstance(capability[field], str) or not capability[field].strip():
                failures.append(f"{label}: {field} must be a non-empty string")
        if capability["direction"] not in ALLOWED_DIRECTIONS:
            failures.append(f"{label}: invalid direction {capability['direction']!r}")
        matches = capability["matches"]
        if not isinstance(matches, list) or not matches:
            failures.append(f"{label}: matches must be a non-empty array")
            continue
        for match in matches:
            if not isinstance(match, dict) or set(match) != {"primitive", "path", "count"}:
                failures.append(f"{label}: invalid match shape")
                continue
            primitive, path, count = match["primitive"], match["path"], match["count"]
            if primitive not in PRIMITIVES:
                failures.append(f"{label}: unknown primitive {primitive!r}")
                continue
            if not isinstance(path, str) or not path.startswith("src/") or not path.endswith(".c"):
                failures.append(f"{label}: invalid source path {path!r}")
                continue
            if type(count) is not int or count < 1:
                failures.append(f"{label}: count must be a positive integer")
                continue
            item = (primitive, path, count)
            if item in expected:
                failures.append(f"{label}: duplicate match {primitive} {path}")
            expected.add(item)
    return expected, failures


def compare(actual: set[tuple[str, str, int]]) -> list[str]:
    expected, failures = expected_contract()
    for primitive, path, count in sorted(actual - expected):
        failures.append(f"unowned network boundary: {primitive} {path} count={count}")
    for primitive, path, count in sorted(expected - actual):
        failures.append(f"stale network contract: {primitive} {path} count={count}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plant-test", action="store_true")
    args = parser.parse_args()
    actual = scan()
    if args.plant_test:
        actual.add(("http", "src/planted/core_egress_bypass.c", 1))
        if not compare(actual):
            print("check-core-network-contracts: planted bypass was not detected", file=sys.stderr)
            return 1
        print("check-core-network-contracts: plant test passed")
        return 0
    failures = compare(actual)
    if failures:
        print("check-core-network-contracts: FAIL", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1
    print(
        "check-core-network-contracts: ok "
        f"({len(actual)} exact file/primitive boundaries constrained)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
