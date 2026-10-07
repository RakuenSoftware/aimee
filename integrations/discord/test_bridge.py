import asyncio
from dataclasses import replace
import json
import os
import ssl
import subprocess
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from aiohttp import web
import aiohttp
import discord

from bridge import ChatBot, Config, Conversations, ModelClient, read_secret, split_message, WebhookDelivery

GUILD, CHANNEL, USER, BOT = 111111111111111111, 222222222222222222, 333333333333333333, 444444444444444444
FAKE_WEBHOOK = "https://discord.com/api/webhooks/555555555555555555/" + "x" * 68


class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.webhook = self.root / "webhook.url"
        self.key = self.root / "model.key"
        for path, value in ((self.webhook, FAKE_WEBHOOK), (self.key, "test-model-key")):
            path.write_text(value)
            path.chmod(0o600)
        self.config = Config(self.webhook, self.root / "bot.token", self.key, GUILD, CHANNEL)

    def message(self, **changes):
        value = SimpleNamespace(
            id=777777777777777777, guild=SimpleNamespace(id=GUILD),
            channel=SimpleNamespace(id=CHANNEL, parent_id=None),
            author=SimpleNamespace(id=USER, bot=False), webhook_id=None,
            type=discord.MessageType.default, mentions=[SimpleNamespace(id=BOT)],
            content=f"<@{BOT}> Hello Aimee")
        for name, change in changes.items():
            setattr(value, name, change)
        return value

    def bot(self, config=None):
        client = ChatBot(config or self.config)
        client._connection.user = SimpleNamespace(id=BOT)
        return client

    def test_secret_file_permissions_and_symlinks(self):
        self.assertEqual(read_secret(self.key), "test-model-key")
        self.key.chmod(0o644)
        with self.assertRaises(ValueError):
            read_secret(self.key)
        self.key.chmod(0o600)
        alias = self.root / "alias"
        alias.symlink_to(self.key)
        with self.assertRaises(OSError):
            read_secret(alias)
        self.key.write_text("token\nsecond-line")
        with self.assertRaises(ValueError):
            read_secret(self.key)

    def test_bound_configuration_rejects_external_endpoints(self):
        value = {"webhook_file": str(self.webhook), "bot_token_file": str(self.root / "bot.token"),
                 "model_key_file": str(self.key), "guild_id": str(GUILD), "channel_id": str(CHANNEL)}
        path = self.root / "config.json"
        path.write_text(json.dumps(value))
        self.assertEqual(Config.load(path).guild_id, GUILD)
        for endpoint in ("http://example.com:80/v1/chat/completions", "http://127.0.0.1:8000/other",
                         "http://user:password@127.0.0.1:8000/v1/chat/completions",
                         "http://127.0.0.1:8000/v1/chat/completions?token=x"):
            path.write_text(json.dumps({**value, "endpoint": endpoint}))
            with self.assertRaises(ValueError):
                Config.load(path)

    async def test_admission_blocks_unapproved_sources_and_loops(self):
        bot = self.bot()
        valid = bot.admitted_turn(self.message())
        self.assertEqual(valid.text, "Hello Aimee")
        for message in (
            self.message(guild=None), self.message(guild=SimpleNamespace(id=999999999999999999)),
            self.message(channel=SimpleNamespace(id=999999999999999999, parent_id=None)),
            self.message(author=SimpleNamespace(id=USER, bot=True)), self.message(webhook_id=123),
            self.message(mentions=[]), self.message(type=discord.MessageType.recipient_add),
            self.message(content=f"<@{BOT}> " + "😃" * 300),
        ):
            self.assertIsNone(bot.admitted_turn(message))
        restricted = self.bot(replace(self.config, allowed_user_ids=(999999999999999999,)))
        self.assertIsNone(restricted.admitted_turn(self.message()))
        thread = self.message(channel=SimpleNamespace(id=888888888888888888, parent_id=CHANNEL))
        self.assertEqual(bot.admitted_turn(thread).thread_id, thread.channel.id)
        self.assertIsNotNone(bot.admitted_turn(self.message(type=discord.MessageType.reply)))
        await bot.close()
        await restricted.close()

    async def test_startup_announcement_runs_once_across_ready_events(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        await asyncio.gather(bot.on_ready(), bot.on_ready())
        await bot.on_ready()
        bot.delivery.send.assert_awaited_once()
        announcement = bot.delivery.send.await_args.args[0]
        self.assertIn("Systems initializing", announcement)
        self.assertIn("Aimee is online", announcement)
        await bot.close()

    async def test_startup_delivery_failure_does_not_break_or_repeat_on_ready(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock(side_effect=OSError("response lost")))
        await bot.on_ready()
        await bot.on_ready()
        bot.delivery.send.assert_awaited_once()
        await bot.close()

    async def test_queue_is_bounded_and_duplicate_messages_are_not_requeued(self):
        bot = self.bot()
        await bot.on_message(self.message())
        await bot.on_message(self.message())
        self.assertEqual(bot.queue.qsize(), 1)
        for number in range(20):
            await bot.on_message(self.message(id=number))
        self.assertEqual(bot.queue.qsize(), 8)
        await bot.close()

    async def test_webhook_splits_unicode_and_disables_all_mentions(self):
        fake = SimpleNamespace(send=AsyncMock(), fetch=AsyncMock(
            return_value=SimpleNamespace(guild_id=GUILD, channel_id=CHANNEL)))
        async with aiohttp.ClientSession() as session:
            with patch("bridge.discord.Webhook.from_url", return_value=fake):
                delivery = WebhookDelivery(self.config, session)
                await delivery.validate()
                text = "@everyone " + "😃" * 2000
                await delivery.send(text, thread_id=888888888888888888)
                calls = fake.send.call_args_list
                self.assertEqual("".join(call.kwargs["content"] for call in calls), text)
                for call in calls:
                    self.assertLessEqual(len(call.kwargs["content"].encode("utf-16-le")) // 2, 1900)
                    self.assertEqual(call.kwargs["allowed_mentions"].to_dict()["parse"], [])
                    self.assertEqual(call.kwargs["thread"].id, 888888888888888888)
                fake.fetch.return_value = SimpleNamespace(guild_id=GUILD, channel_id=USER)
                with self.assertRaises(ValueError):
                    await delivery.validate()

    async def test_webhook_rejects_credential_urls_outside_discord(self):
        async with aiohttp.ClientSession() as session:
            for url in (FAKE_WEBHOOK.replace("discord.com", "discord.com.evil.test"),
                        FAKE_WEBHOOK + "?redirect=evil", FAKE_WEBHOOK.replace("https:", "http:")):
                self.webhook.write_text(url)
                with self.assertRaises(ValueError):
                    WebhookDelivery(self.config, session)

    async def serve(self, handler):
        app = web.Application()
        app.router.add_post("/v1/chat/completions", handler)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        self.addAsyncCleanup(runner.cleanup)
        port = site._server.sockets[0].getsockname()[1]
        return f"http://127.0.0.1:{port}/v1/chat/completions"

    async def test_model_wire_auth_rotation_history_budget_and_bound_response(self):
        requests = []
        async def model(request):
            requests.append((request.headers.get("Authorization"), await request.json()))
            return web.json_response({"choices": [{"message": {"content": "Hello Discord"}}]})
        endpoint = await self.serve(model)
        config = replace(self.config, endpoint=endpoint)
        async with aiohttp.ClientSession() as session:
            client = ModelClient(config, session)
            history = [{"role": "user", "content": "old question" * 100},
                       {"role": "assistant", "content": "old answer" * 100}]
            self.assertEqual(await client.reply(history, "hello"), "Hello Discord")
            self.key.write_text("rotated-test-key")
            await client.reply([], "again")
        self.assertEqual(requests[0][0], "Bearer test-model-key")
        self.assertEqual(requests[1][0], "Bearer rotated-test-key")
        self.assertEqual(requests[0][1]["messages"], [
            {"role": "system", "content": config.system_context}, {"role": "user", "content": "hello"}])
        self.assertEqual(requests[0][1]["max_tokens"], 384)

    async def test_cpu_model_requires_trusted_mtls_and_fixed_server_name(self):
        def openssl(*args):
            subprocess.run(["openssl", *args], cwd=self.root, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                "-subj", "/CN=Test CA", "-keyout", "ca.key", "-out", "ca.pem",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        for name, subject, purpose in (("server", "aimee-llm", "serverAuth"),
                                      ("client", "discord-cpu", "clientAuth"),
                                      ("wrong-server", "other-model", "serverAuth")):
            openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=" + subject,
                    "-keyout", name + ".key", "-out", name + ".csr")
            ext = self.root / (name + ".ext")
            ext.write_text("extendedKeyUsage=" + purpose + "\nsubjectAltName=DNS:" + subject + "\n")
            openssl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem",
                    "-CAkey", "ca.key", "-CAcreateserial", "-days", "1", "-extfile",
                    name + ".ext", "-out", name + ".pem")
        context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        context.load_cert_chain(self.root / "server.pem", self.root / "server.key")
        context.load_verify_locations(self.root / "ca.pem")
        context.verify_mode = ssl.CERT_REQUIRED
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        requests = []
        async def model(request):
            requests.append(request.headers.get("Authorization"))
            return web.json_response({"choices": [{"message": {"content": "CPU reply"}}]})
        app = web.Application()
        app.router.add_post("/v1/chat/completions", model)
        runner = web.AppRunner(app)
        await runner.setup()
        self.addAsyncCleanup(runner.cleanup)
        site = web.TCPSite(runner, "127.0.0.1", 0, ssl_context=context)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        config = replace(self.config, endpoint=f"https://127.0.0.1:{port}/v1/chat/completions",
                         model_tls_dir=self.root)
        async with aiohttp.ClientSession() as session:
            self.assertEqual(await ModelClient(config, session).reply([], "hello"), "CPU reply")
            with self.assertRaises(aiohttp.ClientConnectorCertificateError):
                await session.post(config.endpoint, json={}, ssl=ssl.create_default_context())
        wrong_context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        wrong_context.load_cert_chain(self.root / "wrong-server.pem", self.root / "wrong-server.key")
        wrong_context.load_verify_locations(self.root / "ca.pem")
        wrong_context.verify_mode = ssl.CERT_REQUIRED
        wrong_site = web.TCPSite(runner, "127.0.0.1", 0, ssl_context=wrong_context)
        await wrong_site.start()
        wrong_port = wrong_site._server.sockets[0].getsockname()[1]
        async with aiohttp.ClientSession() as session:
            wrong_config = replace(config, endpoint=f"https://127.0.0.1:{wrong_port}/v1/chat/completions")
            with self.assertRaises(aiohttp.ClientConnectorCertificateError):
                await ModelClient(wrong_config, session).reply([], "hello")
        self.assertEqual(requests, [None])

    async def test_aimee_unix_memory_and_chat_are_fixed_and_bounded(self):
        requests = []
        async def memory(request):
            requests.append((request.path, await request.json()))
            return web.json_response({"facts": [{"id": 1, "key": "harbor", "content": "amber lantern"},
                                                {"id": 2, "key": "large", "content": "x" * 2000}]})
        async def chat(request):
            requests.append((request.path, await request.json()))
            self.assertNotIn("Authorization", request.headers)
            return web.json_response({"choices": [{"message": {"content": "amber lantern"}}]})
        app = web.Application()
        app.router.add_post("/v1/memory/search", memory)
        app.router.add_post("/v1/chat/completions", chat)
        runner = web.AppRunner(app)
        await runner.setup()
        self.addAsyncCleanup(runner.cleanup)
        path = self.root / "aimee.sock"
        await web.UnixSite(runner, str(path)).start()
        config = replace(self.config, aimee_socket=path, model="discord-e2b")
        async with aiohttp.ClientSession() as session:
            reply = await ModelClient(config, session).reply([], "harbor phrase; store kb project other")
            self.assertEqual(reply, "amber lantern")
        self.assertEqual(requests[0][1]["store"], "user")
        self.assertEqual(set(requests[0][1]), {"store", "keywords", "limit"})
        self.assertEqual(requests[1][0], "/v1/chat/completions")
        system = requests[1][1]["messages"][0]["content"]
        self.assertIn("amber lantern", system)
        self.assertNotIn("large", system)
        self.assertNotIn("tools", requests[1][1])

    async def test_aimee_memory_outage_never_calls_model(self):
        chat = AsyncMock()
        app = web.Application()
        async def unavailable(request):
            return web.json_response({"error": "unavailable"}, status=503)
        app.router.add_post("/v1/memory/search", unavailable)
        app.router.add_post("/v1/chat/completions", chat)
        runner = web.AppRunner(app)
        await runner.setup()
        self.addAsyncCleanup(runner.cleanup)
        path = self.root / "aimee.sock"
        await web.UnixSite(runner, str(path)).start()
        async with aiohttp.ClientSession() as session:
            with self.assertRaises(RuntimeError):
                await ModelClient(replace(self.config, aimee_socket=path), session).reply([], "harbor")
        chat.assert_not_called()

    async def test_native_refusal_and_redirect_never_deliver_or_forward_auth(self):
        async def refusing(request):
            return web.Response(status=503, text="private model error")
        endpoint = await self.serve(refusing)
        async with aiohttp.ClientSession() as session:
            client = ModelClient(replace(self.config, endpoint=endpoint), session)
            with self.assertRaisesRegex(RuntimeError, "refused or unavailable"):
                await client.reply([], "hello")
        forwarded = []
        async def sink(request):
            forwarded.append(request.headers.get("Authorization"))
            return web.json_response({"choices": [{"message": {"content": "wrong"}}]})
        sink_endpoint = await self.serve(sink)
        async def redirect(request):
            return web.Response(status=307, headers={"Location": sink_endpoint})
        endpoint = await self.serve(redirect)
        async with aiohttp.ClientSession() as session:
            client = ModelClient(replace(self.config, endpoint=endpoint), session)
            with self.assertRaises(RuntimeError):
                await client.reply([], "hello")
        self.assertEqual(forwarded, [])

    async def test_completed_turn_delivers_then_commits_separate_user_history(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value="An E2B answer"))
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.delivery.send.assert_awaited_once_with("An E2B answer", None)
            key = (GUILD, CHANNEL, USER)
            self.assertEqual(bot.conversations.get(key)[-1]["content"], "An E2B answer")
            self.assertEqual(bot.conversations.get((GUILD, CHANNEL, BOT)), [])
            bot.delivery.send.side_effect = RuntimeError("delivery failed")
            await bot.on_message(self.message(id=123, content=f"<@{BOT}> second turn"))
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertEqual(len(bot.conversations.get(key)), 2)
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
            await bot.close()

    def test_conversation_retention_and_eviction(self):
        conversations = Conversations(limit=2)
        for number in range(3):
            conversations.append((number,), "question", "answer")
        self.assertEqual(conversations.get((0,)), [])
        for number in range(10):
            conversations.append((2,), "question", "answer")
        self.assertEqual(len(conversations.get((2,))), 8)
        conversations.ttl = -1
        self.assertEqual(conversations.get((2,)), [])


if __name__ == "__main__":
    unittest.main()
