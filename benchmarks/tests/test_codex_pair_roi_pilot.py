import unittest

from benchmarks.roi.codex_pair_pilot import api_equivalent_cost, summarize


class CodexPairROIPilotTests(unittest.TestCase):
    def test_api_equivalent_cost_prices_distinct_buckets(self):
        usage = {
            "input_tokens": 1000,
            "cached_input_tokens": 400,
            "cache_write_input_tokens": 100,
            "output_tokens": 20,
        }
        self.assertAlmostEqual(api_equivalent_cost(usage), 0.00306)

    def test_summary_preserves_quality_and_thread_lineage(self):
        rows = []
        for condition, input_tokens, thread in (("off", 100, "a"), ("full", 70, "b")):
            usage = {
                "input_tokens": input_tokens,
                "cached_input_tokens": 10,
                "cache_write_input_tokens": 0,
                "output_tokens": 5,
                "reasoning_output_tokens": 0,
            }
            rows.append({
                "condition": condition,
                "resolved": True,
                "thread_id": thread,
                "usage": usage,
                "api_price_equivalent_usd": api_equivalent_cost(usage),
            })
        result = summarize(rows)
        self.assertEqual(result["input_token_delta"], -30)
        self.assertTrue(result["quality_gate_equal_resolved"])
        self.assertTrue(result["unique_threads"])


if __name__ == "__main__":
    unittest.main()
