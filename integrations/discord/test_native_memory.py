"""Owned HTTP transport tests; native datastore semantics have separate Rust tests.

The action fake tests protocol framing and failure handling only. It is not a
replacement datastore, authority service or claimed native end-to-end fixture.
"""
import asyncio
import hashlib
import json
from pathlib import Path
import tempfile
import time
import unittest
from dataclasses import replace
from unittest.mock import AsyncMock, patch
from types import SimpleNamespace

import aiohttp
from aiohttp import web

from native_memory import CONTRACT, AdmissionExpired, AdmittedPayload, NativeConversation, encode

PROJECT = "discord:10000000000000001:20000000000000001"


class ProtocolPeer:
    def __init__(self):
        self.calls, self.completions = [], []
        self.fail_completion = False
        self.prepare_failure = False
        self.alter_completion = False

    async def __call__(self, action, body):
        if action != "memory.conversation" or body["contract"] != CONTRACT or body["project"] != PROJECT:
            raise AssertionError("wrong authenticated protocol framing")
        self.calls.append(body)
        op = body["operation"]
        result = {"status": "ok", "contract": CONTRACT}
        if op == "probe":
            return {**result, "context": "native_exact_sources", "dispatch": "durable_host_fence",
                "outcome": "authenticated_producer_report", "response_capture":"native_input_lineage", "provider_operations":["model_template","model_tokenize","model","discord"]}
        if op == "context":
            return {**result, "context_token": "ab" * 16, "record_owner": "00000000-0000-0000-0000-000000000123", "caller_binding": [3]*32, "text": "Exact native source: é amber", "bytes": 29}
        if op in ("capture", "capture_response"):
            return {**result, "id": "9007199254740993", "reference":{"source":{},"version":{}}}
        if op == "dispatch_prepare":
            if self.prepare_failure:
                raise RuntimeError("lost prepare acknowledgement")
            completion = {"contract": CONTRACT, "project": PROJECT, "record_owner": "00000000-0000-0000-0000-000000000123", "caller_binding": [3]*32, "request_id": body["request_id"],
                "purpose": body["purpose"], "payload_digest": list(hashlib.sha256(body["payload"].encode()).digest()),
                "dispatch_nonce": body["dispatch_nonce"]}
            if self.alter_completion:
                completion["purpose"] = "other"
            return {**result, "completion": completion, "lease_ms": 1000, "send_guard": "acquired", "intent_reference":{"source":{},"version":{}}}
        if op == "dispatch_finish":
            return {**result, "send_guard": "released", "dispatch_state": "finished", "receipt": None}
        if op == "dispatch_complete":
            self.completions.append(body)
            if self.fail_completion:
                raise RuntimeError("completion unavailable")
            return {**result, "send_guard": "released", "receipt": {"attestation": "authenticated_producer_report"}}
        raise AssertionError("unsupported test operation")


class NativeTransportTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.peer = ProtocolPeer()
        self.native = NativeConversation(self.peer, PROJECT, self.directory.name)
        self.requests = []
        self.received = asyncio.Event()
        self.release_response = asyncio.Event()
        self.mode = "normal"
        self.raw_response = encode({"id": "50000000000000001", "choices": [{"message": {"content": "reply"}}]})
        async def provider(request):
            raw = await request.read()
            self.requests.append((raw, dict(request.headers)))
            self.received.set()
            if self.mode == "wait":
                await self.release_response.wait()
            if self.mode == "disconnect":
                request.transport.close()
                return web.Response()
            return web.Response(body=self.raw_response, content_type="application/json")
        app = web.Application()
        app.router.add_post("/provider", provider)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await self.site.start()
        self.url = "http://127.0.0.1:" + str(self.site._server.sockets[0].getsockname()[1]) + "/provider"
        self.session = aiohttp.ClientSession(trust_env=False)

    async def asyncTearDown(self):
        self.release_response.set()
        await self.session.close()
        await self.runner.cleanup()
        self.directory.cleanup()

    async def test_startup_context_exact_http_receive_and_reported_receipt(self):
        await self.native.probe()
        text = await self.native.context("amber", region={"arbitrary-axis": "transport does not reinterpret axes"})
        body = {"messages": [{"role": "system", "content": text}], "large_id": "9007199254740993"}
        value = await self.native.transmit(self.session, self.url, body, purpose="model")
        self.assertEqual(value["id"], "50000000000000001")
        self.assertEqual(self.requests[0][0], encode(body))
        self.assertEqual(self.requests[0][1]["Content-Length"], str(len(encode(body))))
        outcome = self.peer.completions[0]["outcome"]
        self.assertEqual(outcome["state"], "received")
        self.assertEqual(outcome["response_digest"], list(hashlib.sha256(self.raw_response).digest()))
        self.assertFalse(list(Path(self.directory.name).glob("*.json")))
        self.assertEqual(self.peer.calls[1]["region"], {"arbitrary-axis": "transport does not reinterpret axes"})

    async def test_response_retention_is_separate_from_actual_http_delivery(self):
        await self.native.probe()
        turn = SimpleNamespace(guild_id=10000000000000001, channel_id=20000000000000001,
            message_id=30000000000000001, user_id=40000000000000001,
            author_name="producer label", is_bot=False, text="amber")
        await self.native.capture(turn)
        await self.native.context("amber")
        await self.native.transmit(self.session, self.url, {"content":"reply"}, purpose="discord")
        self.assertEqual(len(self.requests), 1)
        report = dict(self.native.last_receipt)
        original = self.native.action
        async def refuse_retention(action, body):
            if body["operation"] == "capture_response":
                raise RuntimeError("native no-broader input policy refused")
            return await original(action, body)
        self.native.action = refuse_retention
        turn.message_id = 50000000000000001
        turn.is_bot = True
        turn.text = "reply"
        with self.assertRaisesRegex(RuntimeError, "no-broader"):
            await self.native.capture_response(turn)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.native.last_receipt, report)
        self.assertEqual(self.native.deliveries[0]["payload"], encode({"content":"reply"}).decode())
        self.assertFalse(list(Path(self.directory.name).glob("*.json")))

    async def test_administrative_unknown_quarantines_exact_descriptor_without_receipt_or_send(self):
        await self.native.context("amber")
        completion={"contract":CONTRACT,"project":PROJECT,"record_owner":self.native.record_owner,
            "caller_binding":self.native.caller_binding,"request_id":"12345678-1234-4234-8234-123456789abc",
            "purpose":"model","payload_digest":[2]*32,"dispatch_nonce":"ab"*16}
        self.native._save({"completion":completion,"outcome":None})
        async def status_only(action, body):
            self.assertEqual(body["operation"], "dispatch_finish")
            return {"status":"ok","contract":CONTRACT,"send_guard":"released_administratively_unknown",
                "dispatch_state":"administrative_unknown","external_outcome":"unknown","receipt":None}
        self.native.action=status_only
        await self.native.recover()
        self.assertEqual(self.requests, [])
        self.assertIsNone(self.native.last_receipt)
        self.assertEqual(self.native.last_recovery,"administrative_unknown")
        paths=list(Path(self.directory.name).glob("*.quarantine"))
        self.assertEqual(len(paths),1)
        value=json.loads(paths[0].read_text())
        self.assertEqual(value["completion"],completion)
        self.assertEqual(value["outcome"]["state"],"unknown")
        self.assertTrue(value["administrative_unknown"])
        self.assertEqual(paths[0].stat().st_mode & 0o777,0o600)
        self.assertFalse(list(Path(self.directory.name).glob("*.json")))
        await self.native.recover()

    async def test_no_context_or_changed_admission_never_sends(self):
        with self.assertRaises(RuntimeError):
            await self.native.transmit(self.session, self.url, {}, purpose="model")
        await self.native.context("amber")
        self.peer.alter_completion = True
        with self.assertRaises(RuntimeError):
            await self.native.transmit(self.session, self.url, {"text": self.native.text}, purpose="model")
        self.assertEqual(self.requests, [])
        self.assertEqual(self.peer.completions[-1]["outcome"]["state"], "not_sent")

    async def test_body_admission_after_connect_and_no_silent_payload_clipping(self):
        class Writer:
            def __init__(self): self.writes = []
            async def write(self, value): self.writes.append(value)
        for method in ("write", "write_with_length"):
            writer, started = Writer(), [False]
            data = AdmittedPayload(b"source", time.monotonic() - 1, started)
            with self.assertRaises(AdmissionExpired):
                if method == "write": await data.write(writer)
                else: await data.write_with_length(writer, 6)
            self.assertFalse(started[0]); self.assertFalse(writer.writes)
        data = AdmittedPayload(b"source", time.monotonic()+1, [False])
        with self.assertRaises(RuntimeError):
            await data.write_with_length(Writer(), 3)

    async def test_lost_prepare_ack_uses_presaved_completion_without_http_retry(self):
        await self.native.context("amber")
        self.peer.prepare_failure = True
        with self.assertRaisesRegex(RuntimeError, "lost prepare"):
            await self.native.transmit(self.session, self.url, {"text": self.native.text}, purpose="model")
        self.assertFalse(self.requests)
        self.assertEqual(self.peer.completions[0]["outcome"]["state"], "not_sent")
        self.assertFalse(list(Path(self.directory.name).glob("*.json")))

    async def test_unknown_network_outcome_spools_and_recovers_without_resend(self):
        await self.native.context("amber")
        self.mode = "disconnect"
        self.peer.fail_completion = True
        with self.assertRaises(Exception):
            await self.native.transmit(self.session, self.url, {"text": self.native.text}, purpose="model")
        self.assertEqual(len(self.requests), 1)
        paths = list(Path(self.directory.name).glob("*.json"))
        self.assertEqual(len(paths), 1)
        raw = paths[0].read_text()
        self.assertNotIn(self.native.text, raw)
        self.assertEqual(json.loads(raw)["outcome"]["state"], "unknown")
        self.peer.fail_completion = False
        restarted = NativeConversation(self.peer, PROJECT, self.directory.name)
        await restarted.probe()
        self.assertEqual(len(self.requests), 1)
        self.assertFalse(list(Path(self.directory.name).glob("*.json")))
        self.assertIsNone(restarted.context_token)

    async def test_cancelled_after_body_retains_actual_unknown_outcome(self):
        await self.native.context("amber")
        self.mode = "wait"
        send = asyncio.create_task(self.native.transmit(self.session, self.url, {"text": self.native.text}, purpose="model"))
        await asyncio.wait_for(self.received.wait(), 5)
        send.cancel()
        with self.assertRaises(asyncio.CancelledError): await send
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.peer.completions[-1]["outcome"]["state"], "unknown")
        self.assertFalse(list(Path(self.directory.name).glob("*.json")))

    async def test_second_client_cannot_reconcile_an_inflight_http_attempt(self):
        await self.native.context("amber")
        self.mode = "wait"
        send = asyncio.create_task(self.native.transmit(self.session, self.url, {"text": self.native.text}, purpose="model"))
        await asyncio.wait_for(self.received.wait(), 5)
        second = NativeConversation(self.peer, PROJECT, self.directory.name)
        startup = asyncio.create_task(second.probe())
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        self.assertFalse(startup.done())
        self.assertFalse(self.peer.completions)
        self.release_response.set()
        await send
        await startup
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(len(self.peer.completions), 1)

    async def test_malformed_provider_response_is_recorded_not_retried(self):
        await self.native.context("amber")
        self.raw_response = b"[]"
        with self.assertRaisesRegex(RuntimeError, "malformed JSON"):
            await self.native.transmit(self.session, self.url, {"text": self.native.text}, purpose="model")
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.peer.completions[-1]["outcome"]["state"], "received")
        self.assertEqual(self.peer.completions[-1]["outcome"]["status_code"], 200)

    async def test_restart_refuses_unsafe_spool_and_cannot_inherit_source_cache(self):
        file = Path(self.directory.name) / "unsafe.json"
        file.write_text("{}")
        file.chmod(0o644)
        with self.assertRaisesRegex(RuntimeError, "unsafe"):
            await self.native.recover()
        file.unlink()
        restarted = NativeConversation(self.peer, PROJECT, self.directory.name)
        with self.assertRaisesRegex(RuntimeError, "captured"):
            await restarted.transmit(self.session, self.url, {"text":"old memory"}, purpose="discord")

class NativeBridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from bridge import Config
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.key = self.root / "key"
        self.key.write_text("test-only-token")
        self.key.chmod(0o600)
        self.config = Config(self.root / "webhook", self.key, self.key,
            10000000000000001, 20000000000000001, memory_profile="aims-native-v1",
            knowledge_endpoint="http://127.0.0.1:8741/v1/actions", knowledge_key_file=self.key,
            native_state_directory=self.root / "native", system_context="Bounded instructions")

    async def asyncTearDown(self):
        self.directory.cleanup()

    def test_native_configuration_requires_direct_tls_and_durable_completion_path(self):
        from bridge import Config
        value = {"webhook_file": str(self.key), "bot_token_file": str(self.key), "model_key_file": str(self.key),
            "guild_id": "10000000000000001", "channel_id": "20000000000000001",
            "memory_profile": "aims-native-v1", "knowledge_endpoint": "http://127.0.0.1:8741/v1/actions",
            "knowledge_key_file": str(self.key), "native_state_directory": str(self.root / "state"),
            "endpoint": "https://127.0.0.1:18764/v1/chat/completions", "model_tls_dir": str(self.root / "tls"),
            "tokenizer_endpoint": "https://127.0.0.1:18764", "tokenizer_tls_dir": str(self.root / "tls")}
        file = self.root / "config.json"
        file.write_text(json.dumps(value))
        self.assertEqual(Config.load(file).memory_profile, "aims-native-v1")
        for changes in ({"aimee_socket": str(self.root / "aimee.sock")}, {"native_state_directory": "relative"},
                        {"knowledge_endpoint": None}, {"tokenizer_endpoint": None},
                        {"model_tls_dir": None, "endpoint": "http://127.0.0.1:18764/v1/chat/completions"}):
            file.write_text(json.dumps({**value, **changes}))
            with self.assertRaises(ValueError): Config.load(file)

    async def test_actual_model_wrapper_uses_native_transport_without_local_history_replay(self):
        from bridge import ModelClient
        client = ModelClient(self.config, None)
        native = SimpleNamespace(context=AsyncMock(return_value="Exact native source"), transmit=AsyncMock(return_value={
            "usage": {"prompt_tokens": 5}, "choices": [{"message": {"content": "A bounded reply."}}]}))
        client.native_conversation = lambda channel_id=None: native
        client.prompt_budget.fit = AsyncMock(return_value={"prompt_tokens": 5})
        reply = await client.reply([{"role":"assistant", "content":"old revoked memory must not be replayed"}], "latest question")
        self.assertEqual(reply, "A bounded reply.")
        body = native.transmit.call_args.args[2]
        self.assertEqual(len(body["messages"]), 2)
        self.assertNotIn("old revoked memory", json.dumps(body))
        self.assertIn("Exact native source", body["messages"][0]["content"])
        self.assertNotIn("Confirm matching facts", body["messages"][0]["content"])
        self.assertNotIn("latest authenticated human constraint", body["messages"][0]["content"])
        self.assertEqual(native.transmit.call_args.kwargs["purpose"], "model")
        with self.assertRaisesRegex(RuntimeError, "refused"):
            await client.reply([], "latest", memory_unavailable=True)
        self.assertEqual(native.transmit.await_count, 1)

    async def test_actual_tokenizer_wrapper_routes_source_bytes_through_active_native_context(self):
        from bridge import ModelClient
        config = replace(self.config, tokenizer_endpoint="https://127.0.0.1:18764", tokenizer_tls_dir=self.root / "tls")
        client = ModelClient(config, None)
        active = SimpleNamespace(transmit=AsyncMock(return_value={"prompt":"Exact native source", "tokens":[1]}))
        client.native_memory = active
        client.native_conversation = lambda channel=None: self.fail("tokenizer must not remap the active thread to default channel")
        tls = SimpleNamespace(verify_flags=0, minimum_version=None, load_cert_chain=lambda *args: None)
        with patch("bridge.ssl.create_default_context", return_value=tls):
            await client.tokenizer_request("/apply-template", {"messages":[{"role":"system","content":"Exact native source"}]})
            self.assertEqual(active.transmit.call_args.kwargs["purpose"], "model_template")
            await client.tokenizer_request("/tokenize", {"content":"Exact native source"})
            self.assertEqual(active.transmit.call_args.kwargs["purpose"], "model_tokenize")
            with self.assertRaisesRegex(RuntimeError,"lacks actual native context"):
                await client.tokenizer_request("/unknown", {"content":"source"})
        self.assertEqual(active.transmit.await_count, 2)

    async def test_native_event_mapping_does_not_assert_user_authority(self):
        from bridge import Turn
        peer = ProtocolPeer()
        native = NativeConversation(peer, PROJECT, self.root / "state")
        turn = Turn(30000000000000001, 10000000000000001, 20000000000000001, 40000000000000001,
            "producer reported human text", author_name="Name is data")
        self.assertEqual(await native.capture(turn), "9007199254740993")
        args = peer.calls[-1]
        self.assertEqual(args["episode"]["speaker_kind"], "human")
        self.assertNotIn("authority", args)
        self.assertNotIn("principal", args)
        self.assertNotIn("actor_kind", args)

    async def test_actual_discord_sender_frames_exact_body_and_keeps_completion_inside_transport(self):
        from bridge import ChatBot
        bot = ChatBot(self.config)
        native = SimpleNamespace(transmit=AsyncMock(return_value={"id":"50000000000000001"}))
        bot.model_client = SimpleNamespace(native_conversation=lambda channel: native)
        bot.delivery_session = object()
        ids = await bot.send_native_chat_reply(20000000000000001, "Exact outgoing reply", (40000000000000001,))
        self.assertEqual(ids, [50000000000000001])
        call = native.transmit.call_args
        self.assertEqual(call.args[1], "https://discord.com/api/v10/channels/20000000000000001/messages")
        self.assertEqual(call.args[2], {"content":"Exact outgoing reply", "allowed_mentions": {
            "parse":[], "users":["40000000000000001"], "replied_user":False}})
        self.assertEqual(call.kwargs["purpose"], "discord")


if __name__ == "__main__": unittest.main()
