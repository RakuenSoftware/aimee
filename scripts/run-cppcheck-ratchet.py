#!/usr/bin/env python3
"""Run cppcheck and reject diagnostics beyond the reviewed debt baseline."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "tests/baselines/security/cppcheck-v1.json"


def load_baseline() -> Counter[tuple[str, str]]:
    value = json.loads(BASELINE.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or set(value) != {"schema_version", "diagnostics"}:
        raise ValueError("baseline top level differs from v1")
    if value["schema_version"] != 1 or not isinstance(value["diagnostics"], list):
        raise ValueError("baseline schema_version must be 1 and diagnostics must be an array")
    result: Counter[tuple[str, str]] = Counter()
    for item in value["diagnostics"]:
        if not isinstance(item, dict) or set(item) != {"id", "file", "max_count"}:
            raise ValueError("invalid baseline diagnostic shape")
        identifier, path, count = item["id"], item["file"], item["max_count"]
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("diagnostic id must be a non-empty string")
        if not isinstance(path, str) or path.startswith("/") or ".." in Path(path).parts:
            raise ValueError(f"invalid diagnostic path {path!r}")
        if type(count) is not int or count < 1:
            raise ValueError("diagnostic max_count must be a positive integer")
        key = (identifier, path)
        if key in result:
            raise ValueError(f"duplicate diagnostic baseline {identifier}:{path}")
        result[key] = count
    return result


def parse_report(path: Path) -> tuple[Counter[tuple[str, str]], dict[tuple[str, str], list[str]]]:
    root = ET.parse(path).getroot()
    counts: Counter[tuple[str, str]] = Counter()
    details: dict[tuple[str, str], list[str]] = {}
    errors = root.find("errors")
    if errors is None:
        raise ValueError("cppcheck XML has no errors element")
    for error in errors.findall("error"):
        identifier = error.get("id", "")
        locations = error.findall("location")
        location = locations[0] if locations else None
        source = location.get("file", "") if location is not None else "<no-file>"
        try:
            relative = Path(source).resolve().relative_to(ROOT / "src").as_posix()
        except (OSError, ValueError):
            relative = source.replace("\\", "/")
            if relative.startswith("src/"):
                relative = relative[4:]
        key = (identifier, relative)
        counts[key] += 1
        line = location.get("line", "?") if location is not None else "?"
        message = error.get("verbose") or error.get("msg") or ""
        details.setdefault(key, []).append(f"{relative}:{line}: [{identifier}] {message}")
    return counts, details


def excess(
    actual: Counter[tuple[str, str]], baseline: Counter[tuple[str, str]]
) -> list[tuple[tuple[str, str], int, int]]:
    return [
        (key, count, baseline.get(key, 0))
        for key, count in sorted(actual.items())
        if count > baseline.get(key, 0)
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("sources", nargs="*")
    args = parser.parse_args()
    try:
        baseline = load_baseline()
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"cppcheck-ratchet: invalid baseline: {exc}", file=sys.stderr)
        return 2
    if args.self_test:
        planted = baseline.copy()
        planted[("plantedSecurityFinding", "planted/bypass.c")] = 1
        if not excess(planted, baseline):
            print("cppcheck-ratchet: planted diagnostic was not detected", file=sys.stderr)
            return 1
        print("cppcheck-ratchet: plant test passed")
        return 0
    if not args.sources:
        parser.error("at least one source is required")
    with tempfile.NamedTemporaryFile(prefix="aimee-cppcheck-", suffix=".xml") as report:
        command = [
            "cppcheck",
            "-j",
            str(max(1, os.cpu_count() or 2)),
            "--enable=warning,performance,portability",
            "--suppress=missingIncludeSystem",
            "--suppress=nullPointerOutOfMemory",
            "--suppress=normalCheckLevelMaxBranches",
            "--xml",
            "--xml-version=2",
            "-Iheaders",
            "-Ivendor/headers",
            *args.sources,
        ]
        completed = subprocess.run(command, stderr=report, check=False)
        if completed.returncode != 0:
            print(f"cppcheck-ratchet: analyzer exited {completed.returncode}", file=sys.stderr)
            return completed.returncode
        report.flush()
        try:
            actual, details = parse_report(Path(report.name))
        except (ET.ParseError, OSError, ValueError) as exc:
            print(f"cppcheck-ratchet: invalid analyzer report: {exc}", file=sys.stderr)
            return 2
    failures = excess(actual, baseline)
    if failures:
        print("cppcheck-ratchet: new or increased diagnostics", file=sys.stderr)
        for key, count, allowed in failures:
            print(f"  {key[0]}:{key[1]} count={count} baseline={allowed}", file=sys.stderr)
            for detail in details.get(key, [])[:5]:
                print("    " + detail, file=sys.stderr)
        return 1
    known = sum(actual.values())
    ceiling = sum(baseline.values())
    print(f"cppcheck-ratchet: ok ({known} known diagnostics; ceiling {ceiling}; no increase)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
