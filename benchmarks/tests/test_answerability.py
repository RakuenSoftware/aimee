import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from benchmarks.common.runner import compute_longmemeval_derived, write_result_file
from benchmarks.common.result_schema import make_coverage, run_is_complete
from benchmarks.common.llm_eval import build_answer_prompt, judge_vote
from benchmarks.longmemeval.common.dataset import load_cases as longmem
from benchmarks.locomo.common.dataset import load_cases as locomo


class AnswerabilityTests(unittest.TestCase):
    def test_mixed_confusion_and_undefined_ratios(self):
        rows = [{"answerable": label, "generated_answer": text, "verdict": verdict}
                for label, text, verdict in [(True, "Orion", "CORRECT"), (True, "Unknown", "WRONG"),
                                              (False, "Orion", "WRONG"), (False, "Unknown", "CORRECT")]]
        got = compute_longmemeval_derived(rows)
        self.assertEqual(set(got["confusion"].values()), {1})
        for key in ("factoid_recall", "abstention_precision", "abstention_recall", "unsupported_answer_rate"):
            self.assertEqual(got[key], .5)
        self.assertIsNone(compute_longmemeval_derived(rows[:1])["abstention_precision"])
        self.assertIsNone(compute_longmemeval_derived([])["abstention_recall"])
        self.assertEqual(compute_longmemeval_derived([{}])["missing_answerability_labels"], 1)
        self.assertEqual(compute_longmemeval_derived([{**rows[0], "run_status": "timeout"}])["failed_labelled_cases"], 1)

    def test_inventory_caps_and_missing_results_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "data.json"
            path.write_text(json.dumps([{"question_id": "q_abs", "question": "Where?", "haystack_sessions": [[{"content": "hello"}]], "answer_session_ids": []},
                                        {"question_id": "q2", "question": "When?", "haystack_sessions": [[{"content": "hello"}]]},
                                        {"question_id": "bad", "question": ""}]))
            cases = longmem(str(path))
            self.assertEqual(len(cases), 2)
            self.assertFalse(cases[0]["answerable"])
            self.assertEqual(cases.inventory["cases"][-1]["reason"], "empty_question")
            capped = longmem(str(path), 1)
            self.assertEqual(capped.inventory["cases"][1]["reason"], "max_cases")
            out = Path(directory) / "out.json"
            payload = {"dataset_inventory": cases.inventory, "results": []}
            with self.assertRaises(ValueError):
                write_result_file(out, payload)
            self.assertFalse(out.exists())
            payload["results"] = [{"question_id": c["question_id"], "answerable": c["answerable"]} for c in cases]
            payload["coverage"] = make_coverage(samples_run=2, questions_run=2)
            write_result_file(out, payload)
            self.assertFalse(payload["coverage"]["complete"])
            self.assertFalse(run_is_complete(payload))
            before = out.read_bytes()
            payload["results"].reverse()
            with self.assertRaises(ValueError):
                write_result_file(out, payload)
            self.assertEqual(out.read_bytes(), before)

    def test_locomo_answerability_requires_explicit_label(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "data.json"
            path.write_text(json.dumps([{"conversation": {"session_1": [{"text": "hello"}]}, "qa": [
                {"question": "What never happened?", "category": 5, "evidence": []},
                {"question": "What never happened?", "evidence": []}]}]))
            cases = locomo(str(path))
            self.assertFalse(cases[0]["questions"][0]["answerable"])
            self.assertIsNone(cases[0]["questions"][1]["answerable"])
            self.assertEqual(len(locomo(str(path), 1).inventory["cases"]), 2)

    def test_judge_labels_do_not_enter_reader_and_invalid_judge_fails(self):
        class Harness:
            response = '{"score":1}'
            def agent_run(self, home, **kwargs):
                self.prompt = kwargs["prompt"]
                return SimpleNamespace(response=self.response, latency_s=0, prompt_tokens=1, completion_tokens=1)
        harness = Harness()
        vote = judge_vote(harness, Path("."), question="Where?", gold_answer="CANARY", candidate="Unknown", answerable=False)
        self.assertEqual(vote[0], "CORRECT")
        self.assertIn("unanswerable", harness.prompt)
        self.assertNotIn("CANARY", build_answer_prompt("Where?", "context"))
        for malformed in ('text {"score":1}', '{"score":true}', '{"score":2}', '{}'):
            harness.response = malformed
            with self.assertRaises((ValueError, TypeError)):
                judge_vote(harness, Path("."), question="Where?", gold_answer="x", candidate="y")


if __name__ == "__main__":
    unittest.main()
