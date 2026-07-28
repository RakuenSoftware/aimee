#!/usr/bin/env python3
"""Unit tests for scripts/aimee_llm_gateway.py — the gateway's pure request logic
(validation, caps, typed errors, health aggregation), stdlib-only, plus a
numpy-guarded do_rerank test against a mocked encoder. No model, no network.
"""
import importlib.util
import io
import json
import os
import threading
import unittest
import urllib.error
import urllib.request
from unittest import mock

GW = os.path.join(os.path.dirname(__file__), "..", "aimee_llm_gateway.py")


def _gw():
    spec = importlib.util.spec_from_file_location("aimee_llm_gateway", GW)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class StartupEnvironment(unittest.TestCase):
    def test_empty_stub_dimensions_use_default(self):
        # Compose materializes optional unset values as empty strings. Importing
        # the real-model gateway must not crash on int("").
        with mock.patch.dict(os.environ,
                             {"AIMEE_LLM_STUB_DIM": "", "EMBEDDER_STUB_DIM": ""}):
            self.assertEqual(_gw().STUB_DIM, 1024)

    def test_empty_primary_dimension_uses_embedder_fallback(self):
        with mock.patch.dict(os.environ,
                             {"AIMEE_LLM_STUB_DIM": "", "EMBEDDER_STUB_DIM": "2560"}):
            self.assertEqual(_gw().STUB_DIM, 2560)

    def test_empty_synth_discovery_values_use_defaults(self):
        with mock.patch.dict(os.environ,
                             {"AIMEE_LLM_SYNTH_SLOTS": "", "AIMEE_LLM_SYNTH_CTX": ""}):
            gw = _gw()
            self.assertEqual(gw.SYNTH_SLOTS, 1)
            self.assertEqual(gw.SYNTH_CONTEXT, 32768)


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


class Discovery(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_openai_model_catalog_lists_synth_alias(self):
        self.gw.STUB = True
        catalog = self.gw.model_catalog()
        self.assertEqual(catalog["object"], "list")
        self.assertEqual(catalog["data"][0]["id"], "aimee-synth")

    def test_slots_report_per_request_context(self):
        self.gw.SYNTH_SLOTS = 4
        self.gw.SYNTH_CONTEXT = 1024000
        slots = self.gw.slot_catalog()
        self.assertEqual(len(slots), 4)
        self.assertTrue(all(slot["n_ctx"] == 256000 for slot in slots))


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

    def test_stream_relays_sse_verbatim(self):
        """stream:true is proxied, not rejected. Was a typed 400
        ("streaming_unsupported"), which made the gateway unusable as an OpenAI-chat
        backend for any streaming client."""
        self.gw.STUB = True
        out = []
        n = self.gw.do_synth_stream({"messages": [], "stream": True}, out.append)
        body = b"".join(out)
        # SSE framing survives verbatim: data: lines and a terminal [DONE]
        self.assertTrue(body.startswith(b"data: "))
        self.assertIn(b"[DONE]", body)
        self.assertEqual(n, len(body))  # the audit byte count is the relayed length

    def test_stream_unconfigured_503_before_any_bytes(self):
        """The typed error must be raised BEFORE relaying starts — once the SSE status
        line is committed a JSON error can no longer be sent."""
        self.gw.STUB = False
        self.gw.SYNTH_URL = ""
        wrote = []
        with self.assertRaises(self.gw.GatewayError) as e:
            self.gw.do_synth_stream({"messages": [], "stream": True}, wrote.append)
        self.assertEqual(e.exception.status, 503)
        self.assertEqual(wrote, [])  # nothing was written

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


class ClientDisconnectHandling(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()
        self.gw.BIND = "127.0.0.1"
        self.gw.PORT = 0
        self.gw.STUB = True
        self.server = self.gw.build_server()
        self.addCleanup(self.server.server_close)

    def handler(self, payload, write_error):
        raw = json.dumps(payload).encode("utf-8")
        handler = object.__new__(self.server.RequestHandlerClass)
        handler.path = "/v1/chat/completions"
        handler.headers = {"content-length": str(len(raw))}
        handler.rfile = io.BytesIO(raw)
        handler.wfile = mock.Mock()
        handler.wfile.write.side_effect = write_error
        handler.send_response = mock.Mock()
        handler.send_header = mock.Mock()
        handler.end_headers = mock.Mock()
        return handler

    def test_nonstream_disconnect_does_not_attempt_second_response(self):
        for error in (BrokenPipeError(), ConnectionResetError()):
            with self.subTest(error=type(error).__name__):
                handler = self.handler({"messages": []}, error)
                handler.do_POST()
                handler.send_response.assert_called_once_with(200)
                handler.wfile.write.assert_called_once()

    def test_stream_disconnect_does_not_attempt_json_error(self):
        handler = self.handler({"messages": [], "stream": True}, BrokenPipeError())
        handler.do_POST()
        handler.send_response.assert_called_once_with(200)
        handler.wfile.write.assert_called_once()

    def test_stream_disconnect_does_not_trip_upstream_breaker(self):
        self.gw.STUB = False
        self.gw.SYNTH_URL = "http://synth:8083"

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self, _size):
                return b"data: first chunk\n\n"

        with mock.patch.object(self.gw.urllib.request, "urlopen", return_value=Response()), \
                mock.patch.object(self.gw._synth_breaker, "allow", return_value=True), \
                mock.patch.object(self.gw._synth_breaker, "record") as record:
            with self.assertRaises(self.gw.ClientDisconnected):
                self.gw.do_synth_stream(
                    {"messages": [], "stream": True},
                    mock.Mock(side_effect=self.gw.ClientDisconnected()),
                )
        record.assert_not_called()


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


class MalformedBodyStatus(unittest.TestCase):
    """A body the gateway cannot parse is the CALLER's error, not a fault here.

    It used to fall through to the blanket handler and come back 500 "internal",
    which tells an operator their inference gateway is broken when the request
    was simply malformed — and invites a client to retry a permanent error
    forever. Exercised over real HTTP because the bug lived in the request
    handler, not in the pure functions the rest of this file covers.
    """

    def setUp(self):
        # STUB so no upstream llama-server is needed for the well-formed case.
        with mock.patch.dict(os.environ, {"AIMEE_LLM_STUB": "1", "AIMEE_LLM_PORT": "0"},
                             clear=False):
            self.gw = _gw()
        self.srv = self.gw.build_server()
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def _post(self, path, raw: bytes):
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}", data=raw,
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read()

    def test_malformed_json_is_400_not_500(self):
        for path in ("/v1/chat/completions", "/embed_batch", "/rerank"):
            for raw in (b"{not json", b"{", b"[1,", b'{"a":}'):
                status, body = self._post(path, raw)
                self.assertEqual(
                    status, 400,
                    f"{path} with {raw!r} returned {status}, body={body!r}")
                self.assertEqual(json.loads(body)["error"]["code"], "bad_request")

    def test_well_formed_body_still_succeeds(self):
        status, _ = self._post(
            "/v1/chat/completions",
            json.dumps({"messages": [{"role": "user", "content": "hi"}]}).encode())
        self.assertEqual(status, 200)


class ManagedServiceAuth(unittest.TestCase):
    """Managed setup must prove the KB's actual credential over real HTTP."""

    def setUp(self):
        with mock.patch.dict(
            os.environ,
            {
                "AIMEE_LLM_STUB": "1",
                "AIMEE_LLM_PORT": "0",
                "AIMEE_LLM_BIND": "127.0.0.1",
                "AIMEE_LLM_AUTH_TOKEN": "managed-kb-service-token",
            },
            clear=False,
        ):
            self.gw = _gw()
        self.srv = self.gw.build_server()
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def _verify(self, token=None):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}/auth/verify",
            data=b"{}",
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read())

    def test_only_the_managed_service_bearer_verifies(self):
        self.assertEqual(self._verify()[0], 401)
        self.assertEqual(self._verify("wrong")[0], 401)
        status, body = self._verify("managed-kb-service-token")
        self.assertEqual(status, 200)
        self.assertEqual(body, {"status": "ok", "scope": "curator"})


if __name__ == "__main__":
    unittest.main()
