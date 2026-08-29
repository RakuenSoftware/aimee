#!/usr/bin/env python3
"""Result schema validation tests.

Tests that validate_direct_result, validate_llm_result, and
validate_provenance correctly accept valid results and reject
malformed ones.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from benchmarks.common.result_schema import (
    PROVENANCE_FIELDS,
    make_coverage,
    require_complete_run,
    retrieval_outcome_bucket,
    run_is_complete,
    validate_coverage,
    validate_direct_result,
    validate_llm_result,
    validate_provenance,
    validate_retrieval_assessment,
)


def _valid_direct_row() -> dict:
    return {
        "system": "aimee",
        "track": "direct",
        "git_commit": "abc1234",
        "question_id": "q1",
        "question": "What is the capital of France?",
        "gold_answer": "Paris",
        "verdict": "CORRECT",
        "retrieval_latency_s": 0.05,
        "retrieved_ids": ["id1", "id2"],
        "citations": [],
        "category": "geography",
    }


def _valid_llm_row() -> dict:
    return {
        **_valid_direct_row(),
        "track": "llm",
        "generated_answer": "Paris",
        "judge_votes": ["CORRECT", "CORRECT", "CORRECT"],
        "answer_latency_s": 1.2,
        "judge_latency_s": 0.8,
        "wall_clock_s": 2.1,
        "tokens": {"prompt": 100, "completion": 50},
        "cost": {"usd": 0.001},
    }


def _sample_value(key: str) -> str | int | bool:
    if key == "seed":
        return 42
    if key == "pinned":
        return True
    return "v1.0.0"


def _valid_provenance_payload() -> dict:
    payload = {key: _sample_value(key) for key in PROVENANCE_FIELDS}
    # Override enum-like fields that require specific values.
    payload["environment"] = "container"
    payload["judge_profile"] = "open70b"
    return payload


class DirectResultSchemaTest(unittest.TestCase):
    def test_valid_row_passes(self) -> None:
        validate_direct_result(_valid_direct_row(), "category")

    def test_missing_system_fails(self) -> None:
        row = _valid_direct_row()
        del row["system"]
        with self.assertRaises(ValueError):
            validate_direct_result(row, "category")

    def test_missing_verdict_fails(self) -> None:
        row = _valid_direct_row()
        del row["verdict"]
        with self.assertRaises(ValueError):
            validate_direct_result(row, "category")

    def test_invalid_verdict_fails(self) -> None:
        row = _valid_direct_row()
        row["verdict"] = "MAYBE"
        with self.assertRaises(ValueError):
            validate_direct_result(row, "category")

    def test_wrong_track_fails(self) -> None:
        row = _valid_direct_row()
        row["track"] = "llm"
        with self.assertRaises(ValueError):
            validate_direct_result(row, "category")

    def test_missing_label_field_fails(self) -> None:
        row = _valid_direct_row()
        del row["category"]
        with self.assertRaises(ValueError):
            validate_direct_result(row, "category")

    def test_null_field_fails(self) -> None:
        row = _valid_direct_row()
        row["gold_answer"] = None  # type: ignore[assignment]
        with self.assertRaises(ValueError):
            validate_direct_result(row, "category")

    def test_wrong_type_for_retrieved_ids_fails(self) -> None:
        row = _valid_direct_row()
        row["retrieved_ids"] = "not_a_list"  # type: ignore[assignment]
        with self.assertRaises(ValueError):
            validate_direct_result(row, "category")


class LLMResultSchemaTest(unittest.TestCase):
    def test_valid_llm_row_passes(self) -> None:
        validate_llm_result(_valid_llm_row(), "category")

    def test_missing_generated_answer_fails(self) -> None:
        row = _valid_llm_row()
        del row["generated_answer"]
        with self.assertRaises(ValueError):
            validate_llm_result(row, "category")

    def test_missing_judge_votes_fails(self) -> None:
        row = _valid_llm_row()
        del row["judge_votes"]
        with self.assertRaises(ValueError):
            validate_llm_result(row, "category")

    def test_missing_tokens_fails(self) -> None:
        row = _valid_llm_row()
        del row["tokens"]
        with self.assertRaises(ValueError):
            validate_llm_result(row, "category")

    def test_wrong_type_for_latency_fails(self) -> None:
        row = _valid_llm_row()
        row["answer_latency_s"] = "one second"  # type: ignore[assignment]
        with self.assertRaises(ValueError):
            validate_llm_result(row, "category")


class RetrievalAssessmentSchemaTest(unittest.TestCase):
    @staticmethod
    def assessment() -> dict:
        return {
            "context_sufficiency": "COMPLETE",
            "context_sufficiency_reason": "All cited facts were retrieved.",
            "retrieved_tokens": 120,
            "assembled_context_tokens": 90,
            "sufficient_context_tokens": 70,
            "unsupported_context_rate": 0.0,
            "citation_validity_rate": 1.0,
            "channel_metrics": {
                "semantic_assertion": {"candidate_count": 4, "result_count": 2, "tokens": 70}
            },
        }

    def test_complete_assessment_passes(self) -> None:
        validate_retrieval_assessment(self.assessment())

    def test_partial_instrumentation_fails(self) -> None:
        with self.assertRaises(ValueError):
            validate_retrieval_assessment({"context_sufficiency": "COMPLETE"})

    def test_legacy_token_metrics_alone_pass(self) -> None:
        validate_retrieval_assessment(
            {"retrieved_tokens": 12, "assembled_context_tokens": 8}
        )

    def test_invalid_grade_fails(self) -> None:
        row = self.assessment()
        row["context_sufficiency"] = "GOOD"
        with self.assertRaises(ValueError):
            validate_retrieval_assessment(row)

    def test_sufficiency_and_correctness_are_separate(self) -> None:
        row = {**_valid_direct_row(), **self.assessment()}
        self.assertEqual(retrieval_outcome_bucket(row), "complete_context__correct_answer")
        row["context_sufficiency"] = "INSUFFICIENT"
        self.assertEqual(
            retrieval_outcome_bucket(row),
            "partial_or_insufficient_context__correct_answer",
        )


class ProvenanceSchemaTest(unittest.TestCase):
    def test_valid_full_provenance_passes(self) -> None:
        errors = validate_provenance(_valid_provenance_payload())
        self.assertEqual(errors, [], f"Expected no errors, got {errors}")

    def test_empty_payload_passes(self) -> None:
        errors = validate_provenance({})
        self.assertEqual(errors, [], "Empty payload should be valid (legacy compat)")

    def test_invalid_environment_fails(self) -> None:
        errors = validate_provenance({"environment": "docker"})
        self.assertTrue(
            any("environment" in e for e in errors),
            f"Expected environment error, got {errors}",
        )

    def test_invalid_judge_profile_fails(self) -> None:
        errors = validate_provenance({"judge_profile": "unknown"})
        self.assertTrue(
            any("judge_profile" in e for e in errors),
            f"Expected judge_profile error, got {errors}",
        )

    def test_null_target_system_fails(self) -> None:
        errors = validate_provenance({"target_system": None})  # type: ignore[dict-item]
        self.assertTrue(
            any("null" in e and "target_system" in e for e in errors),
            f"Expected null field error for target_system, got {errors}",
        )

    def test_wrong_type_for_seed_fails(self) -> None:
        errors = validate_provenance({"seed": "42"})  # type: ignore[dict-item]
        self.assertTrue(
            any("seed" in e and "wrong type" in e for e in errors),
            f"Expected type error for seed, got {errors}",
        )


class TestRunCoverage(unittest.TestCase):
    """A subsampled run must not be usable where a full run is required.

    The failure this guards is on record: the reranker investigation measured
    +0.020 on a 600-question subsample and -0.0048 on the full 10,000 - a sign
    flip - and the two result files were indistinguishable.
    """

    def test_uncapped_run_is_complete(self) -> None:
        block = make_coverage(samples_run=500, questions_run=10000)
        validate_coverage(block)
        self.assertTrue(block["complete"])
        self.assertTrue(run_is_complete({"coverage": block}))
        require_complete_run({"coverage": block}, "baseline eligibility")

    def test_capped_samples_is_partial(self) -> None:
        block = make_coverage(max_samples=10, samples_run=10, questions_run=600)
        validate_coverage(block)
        self.assertFalse(block["complete"])
        with self.assertRaises(ValueError) as ctx:
            require_complete_run({"coverage": block}, "baseline eligibility")
        self.assertIn("max_samples=10", str(ctx.exception))

    def test_capped_questions_is_partial(self) -> None:
        block = make_coverage(max_questions=5, samples_run=10, questions_run=50)
        self.assertFalse(block["complete"])
        with self.assertRaises(ValueError):
            require_complete_run({"coverage": block}, "baseline eligibility")

    def test_missing_coverage_is_unknown_not_complete(self) -> None:
        # None, not False: a legacy file may be a full run, but it cannot show it.
        self.assertIsNone(run_is_complete({"result_count": 3}))
        with self.assertRaises(ValueError) as ctx:
            require_complete_run({"result_count": 3}, "baseline eligibility")
        self.assertIn("no coverage block", str(ctx.exception))

    def test_complete_flag_cannot_contradict_limits(self) -> None:
        # The flag a reader trusts must not be settable independently of the
        # caps that determine it.
        forged = make_coverage(max_questions=5, samples_run=1, questions_run=5)
        forged["complete"] = True
        with self.assertRaises(ValueError):
            validate_coverage(forged)

    def test_negative_counts_rejected(self) -> None:
        block = make_coverage(samples_run=1, questions_run=1)
        block["counts"]["questions_run"] = -1
        with self.assertRaises(ValueError):
            validate_coverage(block)

    def test_source_is_named_in_the_error(self) -> None:
        block = make_coverage(max_samples=2, samples_run=2, questions_run=8)
        with self.assertRaises(ValueError) as ctx:
            require_complete_run({"coverage": block}, "comparison", source="lme_lite.json")
        self.assertIn("lme_lite.json", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
