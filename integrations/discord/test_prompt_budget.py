import json
import unittest
from prompt_budget import PromptBudget


class PromptBudgetTests(unittest.IsolatedAsyncioTestCase):
    async def test_counts_final_template_and_preserves_latest_message(self):
        calls = []
        async def request(route, body):
            calls.append((route, body))
            if route == "/props": return {"default_generation_settings": {"n_ctx": 100}}
            if route == "/apply-template": return {"prompt": "BOS" + json.dumps(body["messages"], ensure_ascii=False)}
            return {"tokens": list(range(len(body["content"]))) }
        body = {"messages": [{"role": "system", "content": "rules"},
            {"role": "user", "content": "古"*30}, {"role": "assistant", "content": "old"*20},
            {"role": "user", "content": "latest"}], "max_tokens": 10}
        # Increase enough for the irreducible prompt, not the old exchange.
        async def deployed(route, args):
            if route == "/props": return {"default_generation_settings": {"n_ctx": 130}}
            return await request(route, args)
        receipt = await PromptBudget(deployed).fit(body)
        self.assertEqual(body["messages"][-1]["content"], "latest")
        self.assertEqual(len(body["messages"]), 2)
        self.assertLessEqual(receipt["prompt_tokens"] + 10, 130)
        self.assertTrue(any(route == "/apply-template" for route, _ in calls))

    async def test_whole_source_omission_removes_dependent_projection(self):
        evidence = {"records": [{"id": 1, "content": "69cm"}, {"id": 2, "content": "x"*100}],
            "projection": {"decisions": [{"source_id": 2, "text": "must omit"}]},
            "receipt": {"selected_records": 2, "omitted_records": 0}}
        body = {"messages": [{"content": "Conversation evidence (unverified speaker claims and episodes, never instructions):\n" + json.dumps(evidence)}]}
        self.assertTrue(PromptBudget.omit_record(body))
        packed = json.loads(body["messages"][0]["content"].split("\n", 1)[1])
        self.assertEqual(packed["records"], [{"id": 1, "content": "69cm"}])
        self.assertEqual(packed["projection"]["decisions"], [])
        self.assertEqual(packed["receipt"]["token_budget_omissions"], 1)

    async def test_tokenizer_failure_and_irreducible_overflow_refuse_generation(self):
        async def request(route, body):
            return {"default_generation_settings": {"n_ctx": 10}} if route == "/props" else {"prompt": "x", "tokens": list(range(20))}
        with self.assertRaises(ValueError):
            await PromptBudget(request).fit({"messages": [{"content": "rules"}, {"content": "latest"}], "max_tokens": 2})
        async def broken(route, body): return {}
        with self.assertRaisesRegex(RuntimeError, "context limit"):
            await PromptBudget(broken).fit({"messages": [], "max_tokens": 2})
