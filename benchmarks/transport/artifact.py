#!/usr/bin/env python3
"""Validate transport_benchmark.v1 artifacts and emit latency_slo.v1 input."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "transport_benchmark.v1"
PROFILE_SCHEMA_VERSION = "transport_profiles.v1"
PATHS = {"thin-client-server", "server-kb"}
TREATMENTS = {"baseline", "treatment"}
TIMING_FIELDS = (
    "dns",
    "tcp_connect",
    "tls_handshake",
    "pool_wait",
    "request_write",
    "request_auth",
    "revocation_check",
    "handler_queue",
    "handler",
    "compression_cpu",
    "compression_wall",
    "decompression_cpu",
    "decompression_wall",
    "response_serialize",
    "ttfb",
    "total",
)
BYTE_FIELDS = ("request_raw", "request_wire", "response_raw", "response_wire")
POOL_FIELDS = ("open", "idle", "busy", "draining")
RUNTIME_FIELDS = (
    "kb_accept_backlog",
    "active_handshakes",
    "errors",
    "retries",
    "cpu_percent",
    "rss_bytes",
    "file_descriptors",
)


class ContractError(ValueError):
    pass


def _object(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractError(f"{field} must be an object")
    return value


def _string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ContractError(f"{field} must be a non-empty string")
    return value


def _number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ContractError(f"{field} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise ContractError(f"{field} must be finite and non-negative")
    return result


def _optional_number(value: Any, field: str) -> float | None:
    if value is None:
        return None
    return _number(value, field)


def load_profiles(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ContractError(f"cannot load profiles: {error}") from error
    root = _object(document, "profiles document")
    if root.get("schema_version") != PROFILE_SCHEMA_VERSION:
        raise ContractError(f"profiles schema_version must be {PROFILE_SCHEMA_VERSION!r}")
    profiles = _object(root.get("profiles"), "profiles")
    workloads = _object(root.get("workloads"), "workloads")
    if not profiles or not workloads:
        raise ContractError("profiles and workloads must not be empty")
    for name, profile in profiles.items():
        item = _object(profile, f"profiles.{name}")
        _number(item.get("rtt_ms"), f"profiles.{name}.rtt_ms")
        _optional_number(item.get("down_mbit_s"), f"profiles.{name}.down_mbit_s")
        _optional_number(item.get("up_mbit_s"), f"profiles.{name}.up_mbit_s")
        loss = _number(item.get("loss_percent"), f"profiles.{name}.loss_percent")
        if loss > 100:
            raise ContractError(f"profiles.{name}.loss_percent must be <= 100")
    for name, workload in workloads.items():
        item = _object(workload, f"workloads.{name}")
        concurrency = _number(item.get("concurrency"), f"workloads.{name}.concurrency")
        if not concurrency.is_integer() or concurrency < 1:
            raise ContractError(f"workloads.{name}.concurrency must be a positive integer")
        if item.get("connection_mode") not in {"cold", "warm"}:
            raise ContractError(f"workloads.{name}.connection_mode must be cold or warm")
        _string(item.get("payload"), f"workloads.{name}.payload")
    return root


def _validate_measurements(attempt: dict[str, Any], index: int) -> None:
    prefix = f"attempts[{index}]"
    timings = _object(attempt.get("timings_ms"), f"{prefix}.timings_ms")
    for field in TIMING_FIELDS:
        _optional_number(timings.get(field), f"{prefix}.timings_ms.{field}")
    if set(TIMING_FIELDS) - timings.keys():
        missing = sorted(set(TIMING_FIELDS) - timings.keys())
        raise ContractError(f"{prefix}.timings_ms missing fields: {', '.join(missing)}")
    if timings["total"] is None:
        raise ContractError(f"{prefix}.timings_ms.total must be measured")

    byte_counts = _object(attempt.get("bytes"), f"{prefix}.bytes")
    for field in BYTE_FIELDS:
        value = _number(byte_counts.get(field), f"{prefix}.bytes.{field}")
        if not value.is_integer():
            raise ContractError(f"{prefix}.bytes.{field} must be an integer")

    connection = _object(attempt.get("connection"), f"{prefix}.connection")
    _number(connection.get("age_ms"), f"{prefix}.connection.age_ms")
    request_index = _number(connection.get("request_index"), f"{prefix}.connection.request_index")
    if not request_index.is_integer() or request_index < 1:
        raise ContractError(f"{prefix}.connection.request_index must be a positive integer")
    for field in ("reused", "resumed"):
        if not isinstance(connection.get(field), bool):
            raise ContractError(f"{prefix}.connection.{field} must be boolean")
    _string(connection.get("close_reason"), f"{prefix}.connection.close_reason")

    pool = _object(attempt.get("pool"), f"{prefix}.pool")
    for field in POOL_FIELDS:
        value = _number(pool.get(field), f"{prefix}.pool.{field}")
        if not value.is_integer():
            raise ContractError(f"{prefix}.pool.{field} must be an integer")

    runtime = _object(attempt.get("runtime"), f"{prefix}.runtime")
    for field in RUNTIME_FIELDS:
        _number(runtime.get(field), f"{prefix}.runtime.{field}")

    encoding = _string(attempt.get("content_encoding"), f"{prefix}.content_encoding")
    if encoding not in {"identity", "gzip", "zstd"}:
        raise ContractError(f"{prefix}.content_encoding is unsupported")


def validate(document: dict[str, Any], profiles: dict[str, Any]) -> dict[str, Any]:
    if document.get("schema_version") != SCHEMA_VERSION:
        raise ContractError(f"schema_version must be {SCHEMA_VERSION!r}")
    if document.get("eligibility_set_before_execution") is not True:
        raise ContractError("eligibility_set_before_execution must be true")
    profile = _string(document.get("profile"), "profile")
    workload = _string(document.get("workload"), "workload")
    path = _string(document.get("path"), "path")
    treatment = _string(document.get("treatment"), "treatment")
    if profile not in profiles["profiles"]:
        raise ContractError(f"unknown profile {profile!r}")
    if workload not in profiles["workloads"]:
        raise ContractError(f"unknown workload {workload!r}")
    if path not in PATHS:
        raise ContractError(f"path must be one of {sorted(PATHS)}")
    if treatment not in TREATMENTS:
        raise ContractError(f"treatment must be one of {sorted(TREATMENTS)}")
    _string(document.get("run_id"), "run_id")
    _string(document.get("build_sha"), "build_sha")
    _string(document.get("started_at"), "started_at")

    budget = _object(document.get("budget"), "budget")
    for field in ("p50_ms", "p99_ms", "combined_failure_tail_rate", "confidence"):
        _number(budget.get(field), f"budget.{field}")

    attempts = document.get("attempts")
    if not isinstance(attempts, list):
        raise ContractError("attempts must be an array")
    eligible = 0
    measured = {field: 0 for field in TIMING_FIELDS}
    for index, value in enumerate(attempts):
        attempt = _object(value, f"attempts[{index}]")
        if not isinstance(attempt.get("eligible"), bool):
            raise ContractError(f"attempts[{index}].eligible must be boolean")
        if not isinstance(attempt.get("success"), bool):
            raise ContractError(f"attempts[{index}].success must be boolean")
        if attempt["eligible"]:
            eligible += 1
        _validate_measurements(attempt, index)
        for field, value in attempt["timings_ms"].items():
            if field in measured and value is not None:
                measured[field] += 1
    return {
        "schema_version": SCHEMA_VERSION,
        "valid": True,
        "attempts": len(attempts),
        "eligible_attempts": eligible,
        "timing_coverage": {
            field: (count / len(attempts) if attempts else 0.0)
            for field, count in measured.items()
        },
    }


def to_latency_slo(document: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema_version": "latency_slo.v1",
        "eligibility_set_before_execution": document["eligibility_set_before_execution"],
        "profile": document["profile"],
        "path": document["path"],
        "budget": document["budget"],
        "attempts": [
            {
                "eligible": attempt["eligible"],
                "success": attempt["success"],
                "latency_ms": attempt["timings_ms"]["total"],
            }
            for attempt in document["attempts"]
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument(
        "--profiles",
        type=Path,
        default=Path(__file__).with_name("profiles.json"),
    )
    parser.add_argument("--emit-slo", type=Path)
    args = parser.parse_args()
    try:
        profiles = load_profiles(args.profiles)
        document = json.loads(args.artifact.read_text(encoding="utf-8"))
        root = _object(document, "artifact root")
        result = validate(root, profiles)
        if args.emit_slo:
            args.emit_slo.write_text(
                json.dumps(to_latency_slo(root), indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    except (OSError, json.JSONDecodeError, ContractError) as error:
        print(json.dumps({"schema_version": SCHEMA_VERSION, "valid": False, "error": str(error)}))
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
