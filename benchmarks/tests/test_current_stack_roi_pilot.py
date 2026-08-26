import unittest

from benchmarks.roi.current_stack_pilot import build_tasks, exact_grade, summarize, usage_buckets


class CurrentStackROIPilotTests(unittest.TestCase):
    def test_task_corpus_is_deterministic_and_contains_answer(self):
        first = build_tasks(3)
        second = build_tasks(3)
        self.assertEqual(first, second)
        for task in first:
            self.assertIn(task.expected, str(task.messages))

    def test_fact_position_selects_folded_or_retained_region(self):
        folded = build_tasks(1, "folded")[0]
        tail = build_tasks(1, "tail")[0]
        self.assertIn(folded.expected, folded.messages[3]["content"])
        self.assertIn(tail.expected, tail.messages[19]["content"])
        with self.assertRaises(ValueError):
            build_tasks(1, "elsewhere")

    def test_coordinate_density_changes_folded_workload(self):
        high = build_tasks(1, "tail", "high")[0]
        low = build_tasks(1, "tail", "low")[0]
        self.assertIn("check=00-00-00", high.messages[1]["content"])
        self.assertNotIn("check=", low.messages[1]["content"])
        self.assertIn("queue remained healthy", low.messages[1]["content"])
        with self.assertRaises(ValueError):
            build_tasks(1, "tail", "medium")

    def test_exact_grader_rejects_extra_text_and_wrong_answers(self):
        answers = {"RBK-00-7F39A2", "RBK-01-7F39A2"}
        self.assertTrue(exact_grade("RBK-00-7F39A2", "RBK-00-7F39A2", answers))
        self.assertFalse(exact_grade("answer: RBK-00-7F39A2", "RBK-00-7F39A2", answers))
        self.assertFalse(exact_grade("RBK-01-7F39A2", "RBK-00-7F39A2", answers))

    def test_summary_counts_all_tokens_and_quality(self):
        rows = [
            {"condition": "off", "resolved": True, "usage_reconciled": True,
             "provider_response_id": "a", "usage": {"input_tokens": 100, "output_tokens": 5,
                                                       "total_tokens": 105}},
            {"condition": "full", "resolved": True, "usage_reconciled": True,
             "provider_response_id": "b", "usage": {"input_tokens": 60, "output_tokens": 5,
                                                       "total_tokens": 65}},
        ]
        result = summarize(rows)
        self.assertEqual(result["paired_total_token_delta"], -40)
        self.assertAlmostEqual(result["paired_total_token_reduction_pct"], 40 / 105 * 100)
        self.assertTrue(result["quality_gate_equal_resolved"])
        self.assertTrue(result["all_calls_reconciled"])
        self.assertTrue(result["unique_provider_response_ids"])

    def test_usage_buckets_preserve_cache_reads(self):
        result = usage_buckets({"usage": {
            "prompt_tokens": 100,
            "completion_tokens": 5,
            "total_tokens": 105,
            "prompt_tokens_details": {"cached_tokens": 40},
        }})
        self.assertEqual(result["input_uncached_tokens"], 60)
        self.assertEqual(result["input_cached_tokens"], 40)
        with self.assertRaises(RuntimeError):
            usage_buckets({"usage": {
                "prompt_tokens": 10,
                "completion_tokens": 1,
                "total_tokens": 11,
                "prompt_tokens_details": {"cached_tokens": 20},
            }})


if __name__ == "__main__":
    unittest.main()
