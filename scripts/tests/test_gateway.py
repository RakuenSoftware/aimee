#!/usr/bin/env python3
"""Unit tests for scripts/aimee_llm_gateway.py — the gateway's pure request logic
(validation, caps, typed errors, health aggregation), stdlib-only. No model, no
network.
"""
import copy
import importlib.util
import io
import json
import os
import shutil
import socket
import tempfile
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

    def test_embedding_dimension_pin_drives_stub_default(self):
        with mock.patch.dict(
            os.environ,
            {
                "AIMEE_LLM_STUB_DIM": "",
                "EMBEDDER_STUB_DIM": "",
                "AIMEE_EMBEDDING_DIM": "3840",
            },
        ):
            gw = _gw()
            self.assertEqual(gw.EXPECTED_EMBED_DIM, 3840)
            self.assertEqual(gw.STUB_DIM, 3840)

    def test_embedding_dimension_drift_is_rejected(self):
        with mock.patch.dict(
            os.environ,
            {
                "AIMEE_LLM_STUB": "1",
                "AIMEE_LLM_STUB_DIM": "4",
                "AIMEE_EMBEDDING_DIM": "3",
            },
        ):
            gw = _gw()
            with self.assertRaises(gw.GatewayError) as exc:
                gw.do_embed("drift")
            self.assertEqual(exc.exception.status, 503)
            self.assertEqual(exc.exception.body["error"]["code"], "embedding_dim_mismatch")

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


class HealthAggregation(unittest.TestCase):
    def setUp(self):
        self.gw = _gw()

    def test_loading_when_nothing_configured(self):
        self.assertEqual(self.gw.health_state({"embed": None, "synth": None}), "loading")

    def test_ok_when_all_up(self):
        self.assertEqual(self.gw.health_state({"embed": True, "synth": True}), "ok")
        self.assertEqual(self.gw.health_state({"embed": True, "synth": None}), "ok")

    def test_down_when_any_configured_child_down(self):
        self.assertEqual(self.gw.health_state({"embed": True, "synth": False}), "down")


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
        for path in ("/v1/chat/completions", "/embed_batch"):
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
                "AIMEE_EMBEDDING_DIM": "",
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

    def _post(self, path, payload, token=None, content_type="application/json"):
        headers = {"Content-Type": content_type}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=data,
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

    def test_unauthenticated_request_is_rejected_before_body_read(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=2) as conn:
            conn.settimeout(2)
            conn.sendall(
                b"POST /embed HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Length: 10000000\r\n"
                b"Connection: close\r\n\r\n"
            )
            response = conn.recv(4096)
        self.assertIn(b" 401 ", response.split(b"\r\n", 1)[0])

    def test_all_kb_inference_routes_require_the_managed_bearer(self):
        cases = (
            ("/embed", b"hello", "text/plain"),
            ("/embed_batch", ["hello", "world"], "application/json"),
            (
                "/v1/chat/completions",
                {"messages": [{"role": "user", "content": "hello"}]},
                "application/json",
            ),
        )
        for path, payload, content_type in cases:
            with self.subTest(path=path):
                self.assertEqual(self._post(path, payload, content_type=content_type)[0], 401)
                self.assertEqual(
                    self._post(path, payload, token="wrong", content_type=content_type)[0], 401
                )
                status, _ = self._post(
                    path,
                    payload,
                    token="managed-kb-service-token",
                    content_type=content_type,
                )
                self.assertEqual(status, 200)


class EmbedPrefixes(unittest.TestCase):
    """Per-model query/document prefixes at the embed boundary.

    Retrieval embedders are trained asymmetrically. Serving nomic without its card
    prefixes measured 0.5823 NDCG@10 against 0.6075 with them — a regression large
    enough to invert which model wins the selection, and invisible at runtime because
    the vectors stay well-formed. These tests pin the two properties that make the
    failure detectable: the prefix depends on the declared polarity, and an embedder
    nobody has declared prefixes for is refused rather than served bare.
    """

    def _gw_with(self, model, **env):
        env["AIMEE_LLM_EMBED_MODEL"] = model
        with mock.patch.dict(os.environ, env, clear=False):
            return _gw()

    def _capture(self, gw):
        """Record exactly what text reaches the embedder."""
        sent = []

        def fake_embeddings(base_url, inputs):
            sent.append(inputs)
            if isinstance(inputs, str):
                return [0.1, 0.2]
            return [[0.1, 0.2] for _ in inputs]

        gw._embeddings = fake_embeddings
        return sent

    def test_document_and_query_get_their_own_prefix(self):
        gw = self._gw_with("nomic-embed-text-v2-moe")
        sent = self._capture(gw)
        gw.do_embed("hello", "document")
        gw.do_embed("hello", "query")
        self.assertEqual(sent, ["search_document: hello", "search_query: hello"])

    def test_omitted_input_type_is_document_not_bare(self):
        # The whole bug being fixed is bare text reaching a prefix-dependent model,
        # so the default must be a real polarity rather than "no prefix".
        gw = self._gw_with("nomic-embed-text-v2-moe")
        sent = self._capture(gw)
        gw.do_embed("hello")
        self.assertEqual(sent, ["search_document: hello"])

    def test_batch_prefixes_every_input(self):
        gw = self._gw_with("nomic-embed-text-v2-moe")
        sent = self._capture(gw)
        gw.do_embed_batch(["a", "b"], "query")
        self.assertEqual(sent, [["search_query: a", "search_query: b"]])

    def test_unregistered_embedder_is_refused(self):
        gw = self._gw_with("some-unknown-embedder")
        self._capture(gw)
        for call in (lambda: gw.do_embed("hello"),
                     lambda: gw.do_embed_batch(["hello"])):
            with self.assertRaises(gw.GatewayError) as caught:
                call()
            self.assertEqual(caught.exception.status, 503)
            self.assertEqual(caught.exception.body["error"]["code"], "embedder_unregistered")

    def test_declared_empty_prefixes_are_served_bare(self):
        # bekko-a25m's card defines no prefixes. That is a positive declaration and
        # must be distinguishable from an unregistered model.
        gw = self._gw_with("bekko-a25m")
        sent = self._capture(gw)
        gw.do_embed("hello", "query")
        gw.do_embed("hello", "document")
        self.assertEqual(sent, ["hello", "hello"])

    def test_model_id_decoration_still_resolves(self):
        # db2 model records carry an @revision suffix; casing varies between the HF
        # repo name and the config. Neither changes the trained prefixes.
        for model in ("nomic-embed-text-v2-moe@v1", "Nomic-Embed-Text-V2-MoE"):
            gw = self._gw_with(model)
            sent = self._capture(gw)
            gw.do_embed("hello", "query")
            self.assertEqual(sent, ["search_query: hello"], f"for {model!r}")

    def test_bad_input_type_is_a_client_error(self):
        gw = self._gw_with("nomic-embed-text-v2-moe")
        for bad in ("passage", "QUERY", "doc", "0"):
            with self.assertRaises(gw.GatewayError) as caught:
                gw.parse_input_type(bad)
            self.assertEqual(caught.exception.status, 400)
        self.assertEqual(gw.parse_input_type(None), "document")
        self.assertEqual(gw.parse_input_type(""), "document")

    def test_stub_mode_applies_prefixes_and_still_refuses(self):
        # STUB must exercise the same seam, or e2e passes against a laxer contract
        # than production runs.
        gw = self._gw_with("nomic-embed-text-v2-moe", AIMEE_LLM_STUB="1")
        self.assertNotEqual(gw.do_embed("hello", "query"), gw.do_embed("hello", "document"))
        unknown = self._gw_with("some-unknown-embedder", AIMEE_LLM_STUB="1")
        with self.assertRaises(unknown.GatewayError):
            unknown.do_embed("hello")


class EmbedderRegistry(unittest.TestCase):
    """The registry file is the single place per-model facts live.

    Its whole value is that a bad entry stops the gateway instead of producing
    well-formed wrong vectors, so the failure modes are pinned here: a missing field is
    not a default, and a broken file is not an empty registry.
    """

    VALID = {
        "embedders": {
            "some-model": {
                "pooling": "mean",
                "dim": 768,
                "context": 2048,
                "prefixes": {"query": "q: ", "document": "d: "},
            }
        }
    }

    def _written(self, document):
        path = os.path.join(self.tmp, "embedders.json")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(document if isinstance(document, str) else json.dumps(document))
        return path

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.gw = _gw()

    def test_valid_registry_loads_and_normalises_keys(self):
        registry = self.gw.load_embedders(self._written(self.VALID))
        self.assertEqual(sorted(registry), ["some-model"])
        self.assertEqual(registry["some-model"]["prefixes"]["query"], "q: ")

    def test_shipped_registry_is_valid_and_covers_the_default_model(self):
        # The default AIMEE_LLM_EMBED_MODEL must resolve, or every deployment that
        # doesn't override it refuses to embed.
        registry = self.gw.load_embedders()
        self.assertIn(self.gw.embed_model_key(self.gw.EMBED_MODEL), registry)

    def test_missing_required_field_is_refused(self):
        for field in self.gw.EMBEDDER_REQUIRED_FIELDS:
            document = copy.deepcopy(self.VALID)
            del document["embedders"]["some-model"][field]
            with self.assertRaises(self.gw.EmbedderRegistryError, msg=field) as caught:
                self.gw.load_embedders(self._written(document))
            self.assertIn(field, str(caught.exception))

    def test_partial_or_extra_prefix_polarity_is_refused(self):
        # Half a declaration is the dangerous case: it reads as intentional.
        for prefixes in ({"query": "q: "}, {"document": "d: "}, {},
                         {"query": "q: ", "document": "d: ", "passage": "p: "}):
            document = copy.deepcopy(self.VALID)
            document["embedders"]["some-model"]["prefixes"] = prefixes
            with self.assertRaises(self.gw.EmbedderRegistryError, msg=repr(prefixes)):
                self.gw.load_embedders(self._written(document))

    def test_non_string_prefix_is_refused(self):
        document = copy.deepcopy(self.VALID)
        document["embedders"]["some-model"]["prefixes"]["query"] = None
        with self.assertRaises(self.gw.EmbedderRegistryError):
            self.gw.load_embedders(self._written(document))

    def test_malformed_or_empty_file_is_refused(self):
        for document in ("not json", "[]", '{"embedders": {}}', '{"embedders": []}',
                         '{"embedders": {"m": "nope"}}'):
            with self.assertRaises(self.gw.EmbedderRegistryError, msg=document):
                self.gw.load_embedders(self._written(document))

    def test_absent_file_is_refused(self):
        with self.assertRaises(self.gw.EmbedderRegistryError):
            self.gw.load_embedders(os.path.join(self.tmp, "does-not-exist.json"))

    def test_broken_registry_refuses_to_serve_rather_than_serving_bare(self):
        # Import must not raise (the process should report, not traceback), but every
        # embed path must fail closed with the operator's error text.
        path = self._written("not json")
        with mock.patch.dict(os.environ, {"AIMEE_LLM_EMBEDDERS_FILE": path}):
            gw = _gw()
        self.assertIsNotNone(gw.EMBEDDERS_ERROR)
        with self.assertRaises(gw.GatewayError) as caught:
            gw.embed_prefix("query")
        self.assertEqual(caught.exception.status, 503)
        self.assertEqual(caught.exception.body["error"]["code"], "embedder_registry_invalid")

    def test_broken_registry_refuses_startup(self):
        path = self._written("not json")
        with mock.patch.dict(os.environ, {"AIMEE_LLM_EMBEDDERS_FILE": path}):
            gw = _gw()
        with mock.patch.object(gw.sys, "stderr", io.StringIO()) as err:
            self.assertEqual(gw.main(), 2)
        self.assertIn("cannot read embedder registry", err.getvalue())


class ServingIdentity(unittest.TestCase):
    """/health's serving_id is what the kb records against its corpus.

    A dim and a model name both survive a pooling or prefix change, so neither can
    gate a re-embed. serving_id folds those in. The properties that matter: it changes
    when the space changes, it does NOT change on cosmetic decoration, and STUB reports
    it exactly as the real path does — an e2e that skipped it would let a corpus be
    built with no recorded identity.
    """

    def _gw(self, model="nomic-embed-text-v2-moe", **env):
        env["AIMEE_LLM_EMBED_MODEL"] = model
        with mock.patch.dict(os.environ, env, clear=False):
            return _gw()

    def test_encodes_model_pooling_and_prefixes(self):
        gw = self._gw()
        first = gw.serving_id()
        self.assertTrue(first.startswith("nomic-embed-text-v2-moe/"), first)
        # Same model, prefixes changed -> different space -> different id.
        gw.EMBEDDERS["nomic-embed-text-v2-moe"]["prefixes"]["query"] = "other: "
        self.assertNotEqual(gw.serving_id(), first)

    def test_pooling_change_alone_changes_the_identity(self):
        # The exact failure the guard exists for: nomic served with Qwen3's `last`
        # pooling is well-formed, right width, right name, wrong space.
        gw = self._gw()
        before = gw.serving_id()
        gw.EMBEDDERS["nomic-embed-text-v2-moe"]["pooling"] = "last"
        self.assertNotEqual(gw.serving_id(), before)

    def test_decoration_does_not_change_the_identity(self):
        # db2 model records carry @revision; casing varies. Neither changes the space,
        # so neither may trigger a spurious re-embed.
        base = self._gw().serving_id()
        for model in ("nomic-embed-text-v2-moe@v1", "Nomic-Embed-Text-V2-MoE"):
            self.assertEqual(self._gw(model).serving_id(), base, f"for {model!r}")

    def test_unregistered_model_reports_no_identity(self):
        # Empty is "no identity", which the kb guard treats as a no-op. It must not
        # raise out of the /health handler.
        self.assertEqual(self._gw("some-unknown-embedder").serving_id(), "")

    def test_health_reports_it_in_stub_and_real_mode_alike(self):
        stub = self._gw(AIMEE_LLM_STUB="1")
        real = self._gw()
        self.assertEqual(stub.serving_id(), real.serving_id())
        self.assertNotEqual(stub.serving_id(), "")

    def test_stub_health_payload_carries_identity_and_dim(self):
        # STUB is what e2e runs. If its /health omits serving_id, a stub-backed kb
        # records no identity and the guard never engages in e2e.
        gw = self._gw(AIMEE_LLM_STUB="1", AIMEE_LLM_STUB_DIM="768", AIMEE_LLM_PORT="0")
        srv = gw.build_server()
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=10) as resp:
                payload = json.loads(resp.read())
        finally:
            srv.shutdown()
            srv.server_close()
        self.assertEqual(payload["status"], "ok")
        self.assertEqual(payload["dim"], 768)
        self.assertEqual(payload["model"], "nomic-embed-text-v2-moe")
        self.assertEqual(payload["serving_id"], gw.serving_id())


class EmbedPrefixRouting(unittest.TestCase):
    """input_type travels over real HTTP in the query string.

    /embed's body is raw text and /embed_batch's is a bare JSON array, so neither can
    carry the field without breaking the existing contract. Covered over HTTP because
    the request handler splits the query string itself.
    """

    def setUp(self):
        with mock.patch.dict(os.environ,
                             {"AIMEE_LLM_STUB": "1", "AIMEE_LLM_PORT": "0",
                              "AIMEE_LLM_EMBED_MODEL": "nomic-embed-text-v2-moe"},
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
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read())

    def test_query_and_document_differ_over_http(self):
        _, as_query = self._post("/embed?input_type=query", b"hello")
        _, as_doc = self._post("/embed?input_type=document", b"hello")
        _, defaulted = self._post("/embed", b"hello")
        self.assertNotEqual(as_query, as_doc)
        self.assertEqual(as_doc, defaulted, "omitted input_type must mean document")

    def test_batch_honours_input_type_over_http(self):
        _, as_query = self._post("/embed_batch?input_type=query", json.dumps(["hello"]).encode())
        _, as_doc = self._post("/embed_batch?input_type=document", json.dumps(["hello"]).encode())
        self.assertNotEqual(as_query, as_doc)

    def test_unknown_input_type_is_400_over_http(self):
        status, body = self._post("/embed?input_type=passage", b"hello")
        self.assertEqual(status, 400)
        self.assertEqual(body["error"]["code"], "bad_request")

    def test_query_string_does_not_break_route_matching(self):
        # path matching must strip the query string, or /embed?x=1 would 404.
        status, _ = self._post("/embed?input_type=document", b"hello")
        self.assertEqual(status, 200)


if __name__ == "__main__":
    unittest.main()
