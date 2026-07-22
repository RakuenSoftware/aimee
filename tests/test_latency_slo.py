#!/usr/bin/env python3

import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "latency_slo", ROOT / "benchmarks" / "transport" / "latency_slo.py"
)
assert SPEC and SPEC.loader
LATENCY_SLO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LATENCY_SLO)


def artifact(attempts):
    return {
        "schema_version": "latency_slo.v1",
        "eligibility_set_before_execution": True,
        "profile": "loopback/control",
        "path": "thin-client-server",
        "budget": {
            "p50_ms": 10,
            "p99_ms": 20,
            "combined_failure_tail_rate": 0.01,
            "confidence": 0.95,
        },
        "attempts": attempts,
    }


class LatencySloTests(unittest.TestCase):
    def test_nearest_rank(self):
        self.assertEqual(LATENCY_SLO.nearest_rank([4, 1, 3, 2], 0.50), 2)
        self.assertEqual(LATENCY_SLO.nearest_rank([4, 1, 3, 2], 0.99), 4)

    def test_clean_run_passes_and_excludes_ineligible_attempts(self):
        attempts = [
            {"eligible": True, "success": True, "latency_ms": 5}
            for _ in range(10_000)
        ]
        attempts.append({"eligible": False, "success": False, "latency_ms": 999})
        result = LATENCY_SLO.evaluate(artifact(attempts))
        self.assertTrue(result["passed"])
        self.assertEqual(result["eligible_attempts"], 10_000)
        self.assertEqual(result["excluded_attempts"], 1)

    def test_exact_confidence_bound_rejects_one_percent_observed_bad(self):
        attempts = [
            {"eligible": True, "success": index >= 100, "latency_ms": 5}
            for index in range(10_000)
        ]
        result = LATENCY_SLO.evaluate(artifact(attempts))
        self.assertFalse(result["passed"])
        self.assertFalse(result["checks"]["combined_failure_tail_upper"])
        self.assertGreater(result["combined_failure_tail_upper"], 0.01)

    def test_minimum_sample_and_predeclared_eligibility_are_enforced(self):
        with self.assertRaises(LATENCY_SLO.ContractError):
            LATENCY_SLO.evaluate(artifact([]))
        document = artifact(
            [{"eligible": True, "success": True, "latency_ms": 5} for _ in range(10_000)]
        )
        document["eligibility_set_before_execution"] = False
        with self.assertRaises(LATENCY_SLO.ContractError):
            LATENCY_SLO.evaluate(document)

    def test_artifact_cannot_relax_authoritative_transport_budget(self):
        document = artifact(
            [{"eligible": True, "success": True, "latency_ms": 5} for _ in range(10_000)]
        )
        document["budget"]["p99_ms"] = 21
        with self.assertRaises(LATENCY_SLO.ContractError):
            LATENCY_SLO.evaluate(document)


if __name__ == "__main__":
    unittest.main()
