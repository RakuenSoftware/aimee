#!/usr/bin/env python3
"""Validate the agent-facing code-intelligence red/green fixture contract."""

import argparse
import hashlib
import json
import pathlib
import sys


HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FIXTURE_PATH = HERE / "fixtures.json"
REQUIRED_CLASSES = {
    "tool_discovery",
    "project_scope",
    "python_blast_radius",
    "retrieval_abstention",
    "kb_outage",
}


def fail(message: str) -> None:
    print(f"validate-agent-code-intelligence: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def nonempty_string(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def validate_source(source: object, case_id: str) -> None:
    if not isinstance(source, dict):
        fail(f"{case_id}: source must be an object")
    if not nonempty_string(source.get("path")) and not nonempty_string(source.get("command")):
        fail(f"{case_id}: source needs path or command")
    checksum = source.get("sha256")
    if checksum is not None and (
        not isinstance(checksum, str)
        or len(checksum) != 64
        or any(ch not in "0123456789abcdef" for ch in checksum)
    ):
        fail(f"{case_id}: sha256 must be 64 lowercase hex characters")


def verify_source_bytes(source: dict, case_id: str) -> None:
    source_path = pathlib.Path(source["path"])
    if source_path.is_absolute():
        fail(f"{case_id}: verified source paths must be repository-relative")
    resolved = ROOT / source_path
    if not resolved.is_file():
        fail(f"{case_id}: source does not exist: {source_path}")
    actual = hashlib.sha256(resolved.read_bytes()).hexdigest()
    if actual != source.get("sha256"):
        fail(f"{case_id}: source sha256 mismatch: expected {source.get('sha256')}, got {actual}")


def validate(verify_sources: bool = False) -> None:
    with FIXTURE_PATH.open(encoding="utf-8") as handle:
        fixture = json.load(handle)

    if fixture.get("schema_version") != 1:
        fail("schema_version must be 1")

    snapshot = fixture.get("source_snapshot")
    if not isinstance(snapshot, dict):
        fail("source_snapshot must be an object")
    if snapshot.get("complete_cells", 0) >= snapshot.get("expected_cells", 0):
        fail("red snapshot must remain explicitly incomplete")
    if snapshot.get("incomplete") is not True:
        fail("source_snapshot.incomplete must be true")
    for field in ("results_tree_manifest_sha256", "raw_tree_manifest_sha256"):
        digest = snapshot.get(field, "")
        if len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
            fail(f"source_snapshot.{field} is not a sha256")

    cases = fixture.get("cases")
    if not isinstance(cases, list):
        fail("cases must be a list")

    seen_ids = set()
    seen_classes = set()
    for case in cases:
        if not isinstance(case, dict):
            fail("every case must be an object")
        case_id = case.get("id")
        if not nonempty_string(case_id):
            fail("every case needs a non-empty id")
        if case_id in seen_ids:
            fail(f"duplicate case id: {case_id}")
        seen_ids.add(case_id)
        seen_classes.add(case.get("class"))
        validate_source(case.get("source"), case_id)
        if verify_sources:
            verify_source_bytes(case["source"], case_id)
        for field in ("request", "red_observation", "green_contract"):
            if not isinstance(case.get(field), dict) or not case[field]:
                fail(f"{case_id}: {field} must be a non-empty object")

    missing = REQUIRED_CLASSES - seen_classes
    if missing:
        fail(f"missing required case classes: {sorted(missing)}")

    canonical = json.dumps(fixture, sort_keys=True, separators=(",", ":")).encode("utf-8")
    print(
        "validate-agent-code-intelligence: ok "
        f"({len(cases)} cases, fixture sha256={hashlib.sha256(canonical).hexdigest()})"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--verify-sources",
        action="store_true",
        help="recompute sha256 for repository-relative evidence files",
    )
    args = parser.parse_args()
    validate(verify_sources=args.verify_sources)
