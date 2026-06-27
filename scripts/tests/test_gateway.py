#!/usr/bin/env python3
"""Unit tests for scripts/aimee_llm_gateway.py — the gateway's pure request logic
(validation, caps, typed errors, health aggregation), stdlib-only, plus a
numpy-guarded do_rerank test against a mocked encoder. No model, no network.
"""
import importlib.util
import os
import unittest

GW = os.path.join(os.path.dirname(__file__), "..", "aimee_llm_gateway.py")


def _gw():
    spec = importlib.util.spec_from_file_location("aimee_llm_gateway", GW)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class BatchValidation(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_ok(self):
        self.assertEqual(self.gw.validate_batch(["a", "b"], cap=512), ["a", "b"])

    def test_not_a_list(self):
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.validate_batch("nope")
        self.assertEqual(e.exception.status, 400)

    def test_over_cap_413(self):
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.validate_batch(["x"] * 5, cap=4)
        self.assertEqual(e.exception.status, 413)
        self.assertEqual(e.exception.body["error"]["code"], "batch_too_large")


class RerankParse(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_ok_coerces_str(self):
        self.assertEqual(self.gw.parse_rerank_pairs([["q", 5]]), [("q", "5")])

    def test_not_a_list(self):
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.parse_rerank_pairs({"q": "c"})
        self.assertEqual(e.exception.status, 400)

    def test_bad_item_shape(self):
        for bad in ([["q"]], [["q", "c", "x"]], ["flat"]):
            with self.assertRaises(self.gw.GatewayError):
                self.gw.parse_rerank_pairs(bad)

    def test_empty(self):
        self.assertEqual(self.gw.parse_rerank_pairs([]), [])


class HealthAggregation(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_loading_when_nothing_configured(self):
        self.assertEqual(self.gw.health_state({"embed": None, "rerank": None}), "loading")

    def test_ok_when_all_up(self):
        self.assertEqual(self.gw.health_state({"embed": True, "rerank": True}), "ok")
        self.assertEqual(self.gw.health_state({"embed": True, "rerank": None}), "ok")

    def test_down_when_any_configured_child_down(self):
        self.assertEqual(self.gw.health_state({"embed": True, "rerank": False}), "down")


class RerankHandler(unittest.TestCase):
    def setUp(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest("numpy required")
        self.gw = _gw()

    def test_do_rerank_aligned_to_input_order(self):
        import numpy as np

        head_dir = os.path.join(os.path.dirname(__file__), "..", "aimee_llm_rerank_head.py")
        spec = importlib.util.spec_from_file_location("rerank_head", head_dir)
        rh = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rh)
        # head: score == first component of GELU(v) (W2=I, no LN, W4 picks dim 0)
        D = 4
        W4 = np.zeros((1, D), np.float32)
        W4[0, 0] = 1.0
        self.gw._head = rh.EttinRerankHead(np.eye(D, dtype=np.float32), W4, None)
        self.gw.RERANK_HEAD_DIR = "x"  # non-empty so the head isn't re-loaded

        captured = {}

        def fake_embed(base, texts):
            captured["base"] = base
            captured["texts"] = texts
            # encode the candidate's "relevance" in the first component
            vals = {"hot": 9.0, "cold": -3.0, "warm": 1.0}
            return np.array([[vals[t.split("</s>")[1]], 0, 0, 0] for t in texts], np.float32)

        self.gw._embeddings = fake_embed
        scores = self.gw.do_rerank([["q", "hot"], ["q", "cold"], ["q", "warm"]])
        # /rerank returns scores ALIGNED to input order (not sorted)
        self.assertEqual(len(scores), 3)
        self.assertGreater(scores[0], scores[2])  # hot > warm
        self.assertGreater(scores[2], scores[1])  # warm > cold
        self.assertEqual(captured["texts"], ["q</s>hot", "q</s>cold", "q</s>warm"])
        self.assertEqual(captured["base"], self.gw.RERANK_URL)

    def test_do_rerank_empty(self):
        self.assertEqual(self.gw.do_rerank([]), [])


class Synth(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_streaming_unsupported_400(self):
        self.gw.SYNTH_URL = "http://x"
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.do_synth({"messages": [], "stream": True})
        self.assertEqual(e.exception.status, 400)
        self.assertEqual(e.exception.body["error"]["code"], "streaming_unsupported")

    def test_unconfigured_503(self):
        self.gw.SYNTH_URL = ""
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.do_synth({"messages": []})
        self.assertEqual(e.exception.status, 503)

    def test_not_a_dict_400(self):
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.do_synth(["nope"])
        self.assertEqual(e.exception.status, 400)

    def test_proxies_non_streaming(self):
        self.gw.SYNTH_URL = "http://synth:8083"
        captured = {}

        def fake_post(url, payload, timeout=120):
            captured["url"] = url
            captured["payload"] = payload
            return {"choices": [{"message": {"content": "hi"}}]}

        self.gw._http_post_json = fake_post
        out = self.gw.do_synth({"messages": [{"role": "user", "content": "hi"}], "stream": False})
        self.assertEqual(captured["url"], "http://synth:8083/v1/chat/completions")
        self.assertIn("choices", out)


class RoleHealth(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_gated_when_unconfigured(self):
        self.assertEqual(self.gw.role_state("", configured=False), "gated")
        self.assertEqual(self.gw.role_state("http://x", configured=False), "gated")

    def test_ready_loading_down(self):
        import urllib.error
        from unittest import mock

        class FakeResp:
            def __init__(self, status):
                self.status = status

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        cases = [
            (lambda *a, **k: FakeResp(200), "ready"),
            (lambda *a, **k: FakeResp(503), "loading"),
            (mock.Mock(side_effect=urllib.error.HTTPError("u", 503, "loading", {}, None)), "loading"),
            (mock.Mock(side_effect=urllib.error.HTTPError("u", 500, "err", {}, None)), "down"),
            (mock.Mock(side_effect=OSError("refused")), "down"),
        ]
        for fn, expected in cases:
            with mock.patch("urllib.request.urlopen", fn):
                self.assertEqual(self.gw.role_state("http://x"), expected)


class SynthDeviceLost(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_detects_marker(self):
        self.assertTrue(self.gw._is_device_lost(
            'decode() failed: vk::Queue::submit: ErrorDeviceLost'))
        self.assertTrue(self.gw._is_device_lost("VK_ERROR_DEVICE_LOST"))
        self.assertFalse(self.gw._is_device_lost("context shift not supported"))
        self.assertFalse(self.gw._is_device_lost("the device lost connection"))  # loose phrase

    def test_synth_device_lost_only_on_5xx(self):
        import io
        import urllib.error

        def http_err(code):
            return urllib.error.HTTPError(
                "u", code, "e", {}, io.BytesIO(b'{"error":{"message":"ErrorDeviceLost"}}'))

        self.assertTrue(self.gw._is_synth_device_lost(http_err(500)))
        self.assertFalse(self.gw._is_synth_device_lost(http_err(400)))  # 4xx body can't spoof it
        self.assertFalse(self.gw._is_synth_device_lost(OSError("ErrorDeviceLost")))

    def test_error_text_reads_httperror_body(self):
        import io
        import urllib.error
        exc = urllib.error.HTTPError(
            "u", 500, "err", {}, io.BytesIO(b'{"error":{"message":"ErrorDeviceLost"}}'))
        self.assertIn("ErrorDeviceLost", self.gw._error_text(exc))
        self.assertEqual(self.gw._error_text(RuntimeError("device lost")), "device lost")

    def test_restart_rate_limited(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            self.gw._DEVICE_LOST_STATE = d + "/ts"
            self.gw._DEVICE_LOST_MIN_INTERVAL = 180.0
            self.assertTrue(self.gw._device_lost_restart_allowed(1000.0))   # first time: allowed
            self.assertFalse(self.gw._device_lost_restart_allowed(1100.0))  # within window: held
            self.assertTrue(self.gw._device_lost_restart_allowed(1200.0))   # past window: allowed

    def test_do_synth_device_lost_triggers_recovery_and_reraises(self):
        import io
        import urllib.error
        self.gw.SYNTH_URL = "http://synth:8083"
        called = []
        self.gw._handle_device_lost = lambda: called.append(True)

        def boom(url, payload, timeout=120):
            raise urllib.error.HTTPError(
                url, 500, "err", {}, io.BytesIO(b'{"error":{"message":"ErrorDeviceLost"}}'))

        self.gw._http_post_json = boom
        with self.assertRaises(urllib.error.HTTPError):
            self.gw.do_synth({"messages": [{"role": "user", "content": "hi"}]})
        self.assertEqual(called, [True])

    def test_do_synth_other_error_no_recovery(self):
        self.gw.SYNTH_URL = "http://synth:8083"
        called = []
        self.gw._handle_device_lost = lambda: called.append(True)

        def boom(url, payload, timeout=120):
            raise OSError("connection refused")

        self.gw._http_post_json = boom
        with self.assertRaises(OSError):
            self.gw.do_synth({"messages": [{"role": "user", "content": "hi"}]})
        self.assertEqual(called, [])


if __name__ == "__main__":
    unittest.main()
