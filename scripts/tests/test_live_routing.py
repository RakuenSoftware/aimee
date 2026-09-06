import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("live_routing", Path(__file__).resolve().parents[1] / "validation/providers/live-routing.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


class LiveRoutingTests(unittest.TestCase):
    def test_foreground_result(self):
        result = mod.result_object('data: {"progress":"running"}\n\ndata: {"status":"ok","agent":"free"}\n\ndata: [DONE]\n')
        self.assertEqual(result["agent"], "free")

    def test_gate_checks_selected_agent_and_refusal(self):
        class Response:
            def __init__(self, body): self.body = body
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def read(self, limit): return self.body
        cases = [
            {"name":"free", "request":{"role":"summarize"}, "expected_agent":"free"},
            {"name":"threshold", "request":{"role":"code"}, "expected_error":"competence contract"},
        ]
        with patch.object(mod.urllib.request, "urlopen", side_effect=[Response(b'{"status":"ok","agent":"free"}'), Response(b'{"status":"error","error":"no eligible model meets the competence contract"}')]):
            self.assertEqual(len(mod.run_cases("http://localhost", cases, None)), 2)
        with patch.object(mod.urllib.request, "urlopen", return_value=Response(b'{"status":"ok","agent":"expensive"}')):
            with self.assertRaises(AssertionError): mod.run_cases("http://localhost", cases[:1], None)

    def test_durable_jobs_require_terminal_status(self):
        pending = {"status": "ok", "job_id": 7, "job_status": "pending"}
        with patch.object(mod.time, "sleep"):
            replies = iter([
                {"status": "ok", "job_status": "running"},
                {"status": "ok", "job_status": "done", "job": {"agent_name": "free", "result": "ok"}},
            ])
            result = mod.await_result(lambda *_: next(replies), pending)
            self.assertEqual(result["agent"], "free")
            self.assertEqual(result["status"], "ok")
        result = mod.await_result(lambda *_: {"status": "ok", "job_status": "failed", "job": {"result": "competence contract"}}, pending)
        self.assertEqual(result["status"], "error")
        self.assertFalse(result["agent"])
        with self.assertRaises(TimeoutError):
            mod.await_result(lambda *_: pending, pending, timeout=0)
        with self.assertRaises(RuntimeError):
            mod.await_result(lambda *_: {"status": "ok", "job_status": "not_found"}, pending)

    def test_no_pinned_success_or_empty_gate(self):
        with self.assertRaises(ValueError): mod.run_cases("http://localhost", [], None)
        with self.assertRaises(ValueError):
            mod.run_cases("http://localhost", [{"name":"pinned", "request":{"via":"free"}, "expected_agent":"free"}], None)

if __name__ == "__main__": unittest.main()
