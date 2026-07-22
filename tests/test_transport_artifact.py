#!/usr/bin/env python3

import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "transport_artifact", ROOT / "benchmarks" / "transport" / "artifact.py"
)
assert SPEC and SPEC.loader
ARTIFACT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARTIFACT)
PROFILES = ARTIFACT.load_profiles(ROOT / "benchmarks" / "transport" / "profiles.json")


def attempt():
    return {
        "eligible": True,
        "success": True,
        "content_encoding": "identity",
        "timings_ms": {field: 1.0 for field in ARTIFACT.TIMING_FIELDS},
        "bytes": {field: 100 for field in ARTIFACT.BYTE_FIELDS},
        "connection": {
            "age_ms": 1,
            "request_index": 1,
            "reused": False,
            "resumed": False,
            "close_reason": "peer-close",
        },
        "pool": {field: 0 for field in ARTIFACT.POOL_FIELDS},
        "runtime": {field: 0 for field in ARTIFACT.RUNTIME_FIELDS},
    }


def artifact():
    return {
        "schema_version": "transport_benchmark.v1",
        "eligibility_set_before_execution": True,
        "run_id": "test-run",
        "build_sha": "0123456789abcdef",
        "started_at": "2026-07-22T00:00:00Z",
        "profile": "loopback-control",
        "workload": "cold-connection",
        "path": "thin-client-server",
        "treatment": "baseline",
        "budget": {
            "p50_ms": 10,
            "p99_ms": 20,
            "combined_failure_tail_rate": 0.01,
            "confidence": 0.95,
        },
        "attempts": [attempt()],
    }


class TransportArtifactTests(unittest.TestCase):
    def test_profiles_are_machine_readable_and_complete(self):
        self.assertEqual(len(PROFILES["profiles"]), 4)
        self.assertEqual(PROFILES["profiles"]["wan-thin-client"]["rtt_ms"], 80)
        self.assertEqual(PROFILES["workloads"]["concurrent-64"]["concurrency"], 64)

    def test_complete_artifact_validates_and_reports_coverage(self):
        result = ARTIFACT.validate(artifact(), PROFILES)
        self.assertTrue(result["valid"])
        self.assertEqual(result["eligible_attempts"], 1)
        self.assertEqual(result["timing_coverage"]["tls_handshake"], 1.0)

    def test_unmeasured_stage_is_explicit_null_not_an_omitted_field(self):
        document = artifact()
        document["attempts"][0]["timings_ms"]["request_auth"] = None
        result = ARTIFACT.validate(document, PROFILES)
        self.assertEqual(result["timing_coverage"]["request_auth"], 0.0)
        del document["attempts"][0]["timings_ms"]["request_auth"]
        with self.assertRaises(ARTIFACT.ContractError):
            ARTIFACT.validate(document, PROFILES)

    def test_unknown_profiles_and_incomplete_measurements_fail_closed(self):
        document = artifact()
        document["profile"] = "invented"
        with self.assertRaises(ARTIFACT.ContractError):
            ARTIFACT.validate(document, PROFILES)
        document = artifact()
        del document["attempts"][0]["bytes"]["response_wire"]
        with self.assertRaises(ARTIFACT.ContractError):
            ARTIFACT.validate(document, PROFILES)

    def test_latency_slo_projection_uses_total_latency(self):
        document = artifact()
        document["attempts"][0]["timings_ms"]["total"] = 7.5
        projected = ARTIFACT.to_latency_slo(document)
        self.assertEqual(projected["schema_version"], "latency_slo.v1")
        self.assertEqual(projected["attempts"][0]["latency_ms"], 7.5)


if __name__ == "__main__":
    unittest.main()
