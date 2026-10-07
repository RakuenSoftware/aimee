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

from bridge import ChatBot, Config, Conversations, ModelClient, read_secret, split_message, WebhookDelivery, Turn, render_mentions

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

    def test_durable_memory_configuration_requires_local_api_and_private_key(self):
        value = {"webhook_file": str(self.webhook), "bot_token_file": str(self.root / "bot.token"),
                 "model_key_file": str(self.key), "guild_id": str(GUILD), "channel_id": str(CHANNEL),
                 "aimee_socket": str(self.root / "aimee.sock"),
                 "endpoint": "http://127.0.0.1:18743/v1/chat/completions",
                 "knowledge_key_file": str(self.key)}
        path = self.root / "config.json"
        for endpoint in ("http://example.com:18741/v1/actions", "https://127.0.0.1:18741/v1/actions",
                         "http://127.0.0.1:18741/other", "http://127.0.0.1:18741/v1/actions?token=x", None):
            path.write_text(json.dumps({**value, "knowledge_endpoint": endpoint}))
            with self.assertRaises(ValueError):
                Config.load(path)
        path.write_text(json.dumps({**value, "knowledge_endpoint": "http://127.0.0.1:18741/v1/actions"}))
        self.assertEqual(Config.load(path).knowledge_key_file, self.key)

    async def test_delivered_answer_uses_newly_committed_channel_facts(self):
        bot = self.bot(replace(self.config, knowledge_endpoint="http://127.0.0.1:8741/v1/actions",
                               knowledge_key_file=self.key))
        events = []
        async def reply(*args, **kwargs):
            events.append("draft" if not events else "final")
            return "old draft" if len(events) == 1 else "corrected answer"
        async def capture(*args):
            events.append("capture")
            return True
        async def archive(*args): events.append("archive")
        async def delivery(*args): events.append("delivery")
        bot.model_client = SimpleNamespace(reply=AsyncMock(side_effect=reply),
                                          capture_turn=AsyncMock(side_effect=capture),
                                          archive_turn=AsyncMock(side_effect=archive))
        bot.delivery = SimpleNamespace(send=AsyncMock(side_effect=delivery))
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertEqual(events, ["draft", "capture", "final", "archive", "delivery"])
            bot.delivery.send.assert_awaited_once_with("corrected answer", None)
            self.assertEqual(bot.conversations.get((GUILD, CHANNEL))[-1]["content"], "corrected answer")
            self.assertEqual(bot.model_client.archive_turn.await_args.args[1:], ("corrected answer", False, "response"))
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
            await bot.close()

    async def test_capture_failure_prevents_delivery_and_history_commit(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value="answer"),
                                          capture_turn=AsyncMock(side_effect=OSError("memory unavailable")))
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.delivery.send.assert_not_awaited()
            self.assertEqual(bot.conversations.get((GUILD, CHANNEL)), [])
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
            await bot.close()

    def test_confirmed_statements_use_exact_approved_subject_and_value(self):
        memory = "Current channel facts (data, names are distinct):\n" + json.dumps([
            {"subject": "Kibukx", "relation": "has_height", "object": "6 feet"},
            {"subject": "Kibukx mountains", "relation": "has_height", "object": "69 feet"}])
        for statement, expected in (("Kibukx is 6 feet tall.", "height for Kibukx: 6 feet"),
                                    ("The Kibukx mountains are 69 feet tall", "height for Kibukx mountains: 69 feet"),
                                    ("But Kibukx is 6 feet tall!", "height for Kibukx: 6 feet")):
            self.assertIn(expected, ModelClient.confirmed_statement(memory, statement))
        for statement in ("Kibukx is 69 feet tall", "Kibukx mountains are 6 feet tall", "The Andes mountains are 69 feet tall",
                          "Is Kibukx 6 feet tall?", "Kibukx is not 6 feet tall", "Kibukx is 6 feet tall, Andes is 69 feet tall"):
            self.assertIsNone(ModelClient.confirmed_statement(memory, statement))
        self.assertIsNone(ModelClient.confirmed_statement("", "Kibukx is 6 feet tall"))

    async def test_lifting_recall_prioritizes_capacity_over_height_and_keeps_source(self):
        client = ModelClient(replace(self.config, aimee_socket=self.root / "unused.sock",
                                    knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        async def action(name, body):
            self.assertEqual(name, "memory.search_assertions")
            self.assertEqual(body["query"], "kibukx")
            return {"assertions": [
                {"subject": subject, "relation": relation, "object": value,
                 "lifecycle_state": "persistent", "historical": False}
                for subject, relation, value in (("Kibukx", "has_height", "6 feet"),
                    ("Kibukx mountains", "has_height", "69 feet"), ("Kibukx", "can_lift", "500 pounds"))]}
        client.knowledge_action = AsyncMock(side_effect=action)
        client.fact_sources = AsyncMock(return_value=[{"author": f"<@{USER}>", "url": "https://discord.com/channels/1/2/3"}])
        answer = await client.reply([], "How much can Kibukx lift?", CHANNEL)
        self.assertIn("Kibukx can lift 500 pounds", answer)
        self.assertIn(str(USER), answer)
        for statement in ("Kibukx can lift 500 pounds.", "I'm telling you, Kibukx can lift 500 pounds."):
            answer = await client.reply([], statement, CHANNEL)
            self.assertIn("Recorded lifting capacity for Kibukx: 500 pounds", answer)
        memory = await client.memory_context(None, "How much can Kibukx lift?", CHANNEL)
        for text in ("Can Kibukx lift 500 pounds?", "Kibukx can lift 600 pounds", "Kibukx cannot lift 500 pounds", "Kibukx mountains can lift 500 pounds"):
            self.assertIsNone(client.confirmed_statement(memory, text))

    async def test_personal_height_uses_authenticated_speaker_and_public_source(self):
        client = ModelClient(replace(self.config, aimee_socket=self.root / "unused.sock", knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        fact = {"subject": f"<@{USER}>", "relation": "has_height", "object": "6 feet", "lifecycle_state": "persistent", "historical": False}
        client.knowledge_action = AsyncMock(return_value={"assertions": [fact]})
        client.fact_sources = AsyncMock(return_value=[{"author": f"<@{USER+1}>"}])
        turn = Turn(1, GUILD, CHANNEL, USER, "How tall am I?", author_name="Kibukx")
        answer = await client.reply([], turn.text, CHANNEL, turn=turn)
        self.assertIn(f"<@{USER}> is 6 feet tall", answer)
        self.assertIn(f"<@{USER+1}>", answer)
        client.knowledge_action.assert_awaited_once_with("memory.search_assertions", {"query": f"<@{USER}>", "project": client.channel_project(CHANNEL), "include_historical": False, "limit": 4})
        for question in ("how tall do you think I am?", "What is my height?"):
            answer = await client.reply([], question, CHANNEL, turn=replace(turn, text=question))
            self.assertIn(f"<@{USER}> is 6 feet tall", answer)
        memory = "Facts:\n" + json.dumps([fact])
        self.assertIn("Confirmed height", client.confirmed_statement(memory, f"Remember that <@{USER}> is 6 feet tall"))
        self.assertIsNone(client.confirmed_statement(memory, f"How tall is <@{USER+2}>?"))

    def test_config_load_retains_and_validates_peer_bot_ids(self):
        value = {"webhook_file": str(self.webhook), "bot_token_file": str(self.root / "bot.token"),
                 "model_key_file": str(self.key), "guild_id": str(GUILD), "channel_id": str(CHANNEL),
                 "peer_bot_ids": [str(USER+1)]}
        path = self.root / "peers.json"
        path.write_text(json.dumps(value))
        self.assertEqual(Config.load(path).peer_bot_ids, (USER+1,))
        for peers in (["not-an-id"], str(USER+1), [str(USER+1)]*9):
            path.write_text(json.dumps({**value, "peer_bot_ids": peers}))
            with self.assertRaises(ValueError):
                Config.load(path)

    def test_identity_rendering_uses_trusted_ids_and_display_names(self):
        identities = {USER: "Virant", BOT: "Aimee", USER+1: "Samy"}
        text = render_mentions(f"@{USER} Hello @Samy and @Aimee#5282! @999999999999999999", identities)
        self.assertIn(f"<@{USER}>", text)
        self.assertIn(f"<@{USER+1}>", text)
        self.assertIn(f"<@{BOT}>", text)
        self.assertNotIn("999999999999999999", text)
        self.assertEqual(render_mentions(f"<@{USER}>", identities), f"<@{USER}>")

    async def test_peer_routing_requires_human_session_actual_mention_and_preserves_model_authority(self):
        peer = USER+1
        bot = self.bot(replace(self.config, peer_bot_ids=(peer,)))
        author = SimpleNamespace(id=peer, bot=True, display_name="Samy")
        message = self.message(author=author)
        self.assertIsNone(bot.admitted_turn(message))
        bot.peer_sessions[(CHANNEL, peer)] = (float("inf"), 2)
        self.assertTrue(bot.admitted_turn(message).is_bot)
        self.assertEqual(bot.admitted_turn(message).peer_target, peer)
        self.assertIsNone(bot.admitted_turn(self.message(author=author, mentions=[], content="Hello @Aimee#5282")))
        self.assertIsNone(bot.admitted_turn(self.message(author=author, webhook_id=peer)))
        self.assertIsNone(bot.admitted_turn(self.message(author=SimpleNamespace(id=BOT, bot=True))))
        request = self.message(content=f"<@{BOT}> <@{peer}> Talk to each other.",
                               mentions=[SimpleNamespace(id=BOT), author])
        self.assertEqual(bot.admitted_turn(request).peer_target, peer)
        self.assertIsNone(bot.admitted_turn(self.message(content=f"<@{BOT}> How tall is <@{peer}>?", mentions=request.mentions)).peer_target)
        client = ModelClient(replace(self.config, knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        client.archive_turn = AsyncMock()
        client.knowledge_action = AsyncMock()
        self.assertFalse(await client.capture_turn(bot.admitted_turn(message), "A reply"))
        client.archive_turn.assert_awaited_once()
        client.knowledge_action.assert_not_called()
        # Admission consumes the finite budget exactly once, even on duplicates.
        await bot.on_message(message)
        await bot.on_message(message)
        self.assertEqual(bot.peer_sessions[(CHANNEL,peer)][1], 1)
        await bot.on_message(self.message(id=message.id+1, author=author))
        self.assertEqual(bot.peer_sessions[(CHANNEL,peer)][1], 0)
        self.assertIsNone(bot.admitted_turn(self.message(id=message.id+2, author=author)))
        await bot.close()

    async def test_peer_delivery_only_enables_configured_bot_mentions(self):
        peer = USER+1
        webhook = SimpleNamespace(send=AsyncMock())
        config = replace(self.config, peer_bot_ids=(peer,))
        with patch("discord.Webhook.from_url", return_value=webhook):
            async with aiohttp.ClientSession() as session:
                delivery = WebhookDelivery(config,session)
                await delivery.send(f"<@{peer}> Hello",peer_id=peer)
                mentions = webhook.send.call_args.kwargs["allowed_mentions"]
                self.assertEqual([user.id for user in mentions.users], [peer])
                self.assertFalse(mentions.roles)
                self.assertFalse(mentions.everyone)
                with self.assertRaises(ValueError):
                    await delivery.send("Hello",peer_id=USER)
                await delivery.send(f"<@{USER}> Hello")
                self.assertFalse(webhook.send.call_args.kwargs["allowed_mentions"].users)

    async def test_human_peer_request_creates_bounded_session_and_addresses_correct_partner(self):
        peer = USER+1
        bot = self.bot(replace(self.config, peer_bot_ids=(peer,)))
        partner = SimpleNamespace(id=peer, bot=True, display_name="Samy")
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=f"@{peer} Hello! @999999999999999999"),
                                          capture_turn=AsyncMock(return_value=False))
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message(content=f"<@{BOT}> <@{peer}> Say hello to each other.",
                                              mentions=[SimpleNamespace(id=BOT),partner]))
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.delivery.send.assert_awaited_once_with(f"<@{peer}> Hello!", None, peer_id=peer)
            self.assertEqual(bot.peer_sessions[(CHANNEL,peer)][1],2)
            turn = bot.model_client.reply.call_args.kwargs["turn"]
            self.assertEqual(turn.user_id,USER)
            self.assertEqual(turn.mentions,((peer,"Samy"),))
            await bot.on_message(self.message(id=999, author=partner, content=f"<@{BOT}> Hi!"))
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertTrue(bot.model_client.reply.call_args.kwargs["turn"].is_bot)
            self.assertEqual(bot.peer_sessions[(CHANNEL,peer)][1],1)
        finally:
            worker.cancel()
            await asyncio.gather(worker,return_exceptions=True)
            await bot.close()

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

    async def test_social_message_does_not_retrieve_entity_facts(self):
        requests = []
        async def model(request):
            requests.append(await request.json())
            return web.json_response({"choices": [{"message": {"content": "Hello Samy! Kibukx smells."}}]})
        endpoint = await self.serve(model)
        config = replace(self.config, endpoint=endpoint, aimee_socket=self.root / "unused.sock", knowledge_endpoint="http://127.0.0.1:8741/v1/actions")
        async with aiohttp.ClientSession() as session:
            client = ModelClient(config, session)
            client.memory_context = AsyncMock(side_effect=AssertionError("irrelevant fact lookup"))
            text = f"Say hello to <@{USER+1}> and tell <@{USER+2}> he smells."
            with patch("bridge.aiohttp.UnixConnector", side_effect=lambda **kwargs: aiohttp.TCPConnector()):
                await client.reply([], text, CHANNEL)
            client.memory_context.assert_not_awaited()
        self.assertEqual(requests[0]["messages"][-1]["content"], text)
        self.assertTrue(ModelClient.is_social_request(text))
        self.assertFalse(ModelClient.is_social_request("Say hello to Samy and tell him how tall Kibukx is."))
        self.assertFalse(ModelClient.is_social_request("Tell me how much Kibukx can lift?"))

    async def test_peer_message_preserves_second_explicit_recipient(self):
        peer, recipient = USER+1, USER+2
        bot = self.bot(replace(self.config, peer_bot_ids=(peer,)))
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=f"Samy Hello! Kibukx smells."), capture_turn=AsyncMock(return_value=False))
        bot.delivery = SimpleNamespace(send=AsyncMock())
        task = asyncio.create_task(bot.process_turns())
        try:
            await bot.queue.put(Turn(1, GUILD, CHANNEL, USER, "Say hello", mentions=((peer,"Samy"),(recipient,"Kibukx")), peer_target=peer))
            await bot.queue.join()
            bot.delivery.send.assert_awaited_once_with(f"<@{peer}> Hello! <@{recipient}> smells.", None, peer_id=peer)
        finally:
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task

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

    async def test_durable_capture_uses_fixed_channel_scope_and_archives_rejected_claims(self):
        archives, facts, projections = [], [], []
        async def archive(request):
            archives.append(await request.json())
            return web.json_response({"status": "ok", "id": 1})
        async def store(request):
            self.assertEqual(request.headers["Authorization"], "Bearer test-model-key")
            facts.append(await request.json())
            return web.json_response({"status": "ok", "id": 2})
        async def serve(request):
            projections.append(await request.json())
            return web.json_response({"status": "ok", "assertions": [
                {"lifecycle_state": "persistent", "historical": False,
                 "subject": "Kibukx", "relation": "has_height", "object": "6 feet"},
                {"lifecycle_state": "persistent", "historical": False,
                 "subject": "Kibukx mountains", "relation": "has_height", "object": "69 feet"},
                {"lifecycle_state": "superseded", "historical": True,
                 "subject": "Kibukx", "relation": "has_height", "object": "old value"}]})
        app = web.Application()
        app.router.add_post("/v1/memory/store", archive)
        runner = web.AppRunner(app)
        await runner.setup()
        self.addAsyncCleanup(runner.cleanup)
        socket = self.root / "capture.sock"
        await web.UnixSite(runner, str(socket)).start()
        kb = web.Application()
        kb.router.add_post("/v1/actions/memory.store", store)
        kb.router.add_post("/v1/actions/memory.search_assertions", serve)
        kb_runner = web.AppRunner(kb)
        await kb_runner.setup()
        self.addAsyncCleanup(kb_runner.cleanup)
        site = web.TCPSite(kb_runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        config = replace(self.config, aimee_socket=socket, knowledge_key_file=self.key,
                         knowledge_endpoint=f"http://127.0.0.1:{port}/v1/actions")
        async with aiohttp.ClientSession() as session:
            client = ModelClient(config, session)
            await client.capture_turn(Turn(10, GUILD, CHANNEL, USER, "Kibukx is 6 feet tall."), "Understood.")
            await client.capture_turn(Turn(11, GUILD, CHANNEL, USER + 1, "The Andes are 69 feet tall."),
                                      "That information is incorrect. The Andes are much taller.")
            await client.capture_turn(Turn(12, GUILD, CHANNEL, USER, "The Himalayas are 69 feet tall."),
                                      "That’s a common misconception. Their peaks are much taller.")
            context = await client.memory_context(session, "How tall is Kibukx?", CHANNEL)
            thread = CHANNEL + 1
            await client.memory_context(session, "How tall is Kibukx?", thread)
            correction = await client.memory_context(session, "But Kibukx is 6 feet tall, the Kibukx mountains are 69 feet tall.", CHANNEL)
            self.assertEqual(correction, context)
            self.assertEqual({p["query"] for p in projections[2:]}, {"kibukx"})
        self.assertEqual(len(archives), 3)
        self.assertEqual(json.loads(archives[1]["content"])["claim_admission"], "withheld")
        self.assertEqual(len(facts), 1)
        self.assertEqual(facts[0]["content"], "Kibukx is 6 feet tall.")
        self.assertEqual(facts[0]["project"], f"discord:{GUILD}:{CHANNEL}")
        self.assertEqual(facts[0]["idempotency_key"], facts[0]["key"])
        self.assertEqual(projections[0]["project"], facts[0]["project"])
        self.assertEqual(projections[1]["project"], f"discord:{GUILD}:{thread}")
        self.assertIn('"subject":"Kibukx"', context)
        self.assertIn('"subject":"Kibukx mountains"', context)
        self.assertNotIn("old value", context)
        self.assertLess(len(context.encode()), 384)

    async def test_provenance_followup_resolves_durable_authors_without_model_or_new_author_guess(self):
        config = replace(self.config, aimee_socket=self.root / "unused.sock",
                         knowledge_endpoint="http://127.0.0.1:8741/v1/actions", knowledge_key_file=self.key)
        client = ModelClient(config, None)
        project = client.channel_project(CHANNEL)
        message_ids = ["777777777777777777", "888888888888888888"]
        async def action(name, body):
            self.assertEqual(body["project"], project)
            if name == "memory.search_assertions":
                self.assertEqual(body["query"], "kibukx")
                return {"assertions": [
                    {"subject": subject, "relation": "has_height", "object": height,
                     "lifecycle_state": "persistent", "historical": False,
                     "evidence": [{"source_kind": "memory", "source_id": f"memory:{i+1}", "stance": "supports"},
                                  {"source_kind": "memory", "source_id": "memory:999", "stance": "contradicts"}]}
                    for i, (subject, height) in enumerate((("Kibukx", "6 feet"), ("Kibukx mountains", "69 feet")))]}
            self.assertEqual(name, "memory.get")
            i = body["id"] - 1
            self.assertIn(i, (0, 1))
            return {"memory": {"key": f"{project}:{message_ids[i]}", "source_session": project,
                               "use_cases": json.dumps({"source": "discord", "author_id": str(USER+i), "message_id": message_ids[i]})}}
        client.knowledge_action = AsyncMock(side_effect=action)
        history = [{"role": "user", "content": "So how tall is Kibukx? How tall are the Kibukx Mountains?"},
                   {"role": "assistant", "content": "Kibukx is 6 feet tall, and the mountains are 69 feet tall."}]
        for question in ("Where did you get that information from?", "Who did you get that information from?", "Who told you?"):
            answer = await client.reply(history, question, CHANNEL)
            self.assertIn(f"Kibukx: 6 feet — from <@{USER}>", answer)
            self.assertIn(f"Kibukx mountains: 69 feet — from <@{USER+1}>", answer)
            self.assertIn(f"https://discord.com/channels/{GUILD}/{CHANNEL}/{message_ids[0]}", answer)
            history += [{"role": "user", "content": question}, {"role": "assistant", "content": answer}]
        context = await client.memory_context(None, "How tall is Kibukx?", CHANNEL)
        self.assertIn(str(USER), context)
        self.assertIn(str(USER+1), context)
        self.assertLess(len(context.encode()), 384)

    async def test_provenance_never_invents_author_for_transcript_or_mismatched_channel(self):
        client = ModelClient(self.config, None)
        project = client.channel_project(CHANNEL)
        record = {"evidence": [{"source_kind": "memory", "source_id": "memory:1", "stance": "supports"}]}
        for memory in ({"use_cases": json.dumps({"source": "operator-provided conversation excerpt"})},
                       {"key": f"discord:{GUILD}:{CHANNEL+1}:777777777777777777", "source_session": project,
                        "use_cases": json.dumps({"source": "discord", "author_id": str(USER), "message_id": "777777777777777777"})}):
            client.knowledge_action = AsyncMock(return_value={"memory": memory})
            sources = await client.fact_sources(record, CHANNEL)
            self.assertNotIn("author", sources[0])
            answer = client.provenance_reply("Facts:\n" + json.dumps([{"subject": "Kibukx", "object": "6 feet", "sources": sources}]))
            self.assertIn("original author was not recorded", answer)
            self.assertNotIn(str(USER), answer)
            client.knowledge_action.assert_awaited_once_with("memory.get", {"id": 1, "project": project})
        self.assertEqual(client.provenance_query([], "Who told you?"), "")
        self.assertIn("Which fact", client.provenance_reply(""))

    def test_admitted_turn_retains_authenticated_discord_author_name(self):
        bot = self.bot()
        turn = bot.admitted_turn(self.message(author=SimpleNamespace(id=USER, bot=False, display_name="Virant")))
        self.assertEqual((turn.user_id, turn.author_name), (USER, "Virant"))

    async def test_spatial_retrieval_keeps_lowercase_subjects_beside_named_reference(self):
        client = ModelClient(replace(self.config, knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        queries = []
        async def action(name, body):
            self.assertEqual(name, "memory.search_assertions")
            queries.append(body["query"])
            assertions = [
                {"subject": "Distance between <@333333333333333333>'s house and car wash", "relation": "has_distance", "object": "200 meters", "lifecycle_state": "persistent", "historical": False},
                {"subject": "car wash", "relation": "located_in", "object": "Kansas City, Kansas", "lifecycle_state": "persistent", "historical": False}]
            return {"assertions": assertions if body["query"] in ("car", "wash", "car wash") else assertions[:1] if "house" in body["query"] else []}
        client.knowledge_action = AsyncMock(side_effect=action)
        context = await client.memory_context(None, "Where is <@333333333333333333>'s house? How far away is the car wash from the Himalayas?", CHANNEL)
        self.assertIn("himalayas", queries)
        self.assertIn("car", queries)
        self.assertIn("house", queries)
        self.assertIn("Kansas City, Kansas", context)
        rows = json.loads(context.split("\n", 1)[1])
        distance = next(row for row in rows if row["relation"] == "has_distance")
        self.assertIn("333333333333333333", distance["subject"])
        self.assertNotIn("Himalayas", distance["subject"])
        self.assertLess(len(context.encode()), 384)
        house_context = await client.memory_context(None, "Where is <@333333333333333333>'s house?", CHANNEL)
        self.assertIn("Kansas City, Kansas", house_context)
        self.assertIn("car wash", queries)

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

    async def test_channel_history_is_shared_across_users_and_committed_after_delivery(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value="An E2B answer"), capture_turn=AsyncMock())
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.delivery.send.assert_awaited_once_with("An E2B answer", None)
            key = (GUILD, CHANNEL)
            self.assertEqual(bot.conversations.get(key)[-1]["content"], "An E2B answer")
            shared_history = bot.conversations.get(key)
            await bot.on_message(self.message(
                id=123, author=SimpleNamespace(id=USER + 1, bot=False),
                content=f"<@{BOT}> What did we just discuss?"))
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.model_client.reply.assert_awaited_with(shared_history, "What did we just discuss?", channel_id=CHANNEL,
                                                      turn=Turn(123, GUILD, CHANNEL, USER+1, "What did we just discuss?"))
            self.assertEqual(len(bot.conversations.get(key)), 4)

            thread_id = 888888888888888888
            await bot.on_message(self.message(
                id=124, channel=SimpleNamespace(id=thread_id, parent_id=CHANNEL),
                content=f"<@{BOT}> New thread"))
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.model_client.reply.assert_awaited_with([], "New thread", channel_id=thread_id,
                                                      turn=Turn(124, GUILD, thread_id, USER, "New thread", thread_id))
            bot.delivery.send.assert_awaited_with("An E2B answer", thread_id)
            thread_history = bot.conversations.get((GUILD, thread_id))
            await bot.on_message(self.message(
                id=125, author=SimpleNamespace(id=USER + 1, bot=False),
                channel=SimpleNamespace(id=thread_id, parent_id=CHANNEL),
                content=f"<@{BOT}> Continue this thread"))
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.model_client.reply.assert_awaited_with(thread_history, "Continue this thread", channel_id=thread_id,
                                                      turn=Turn(125, GUILD, thread_id, USER+1, "Continue this thread", thread_id))
            self.assertEqual(len(bot.conversations.get(key)), 4)
            self.assertEqual(bot.conversations.get((GUILD + 1, CHANNEL)), [])
            bot.delivery.send.side_effect = RuntimeError("delivery failed")
            await bot.on_message(self.message(id=126, content=f"<@{BOT}> failed turn"))
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertEqual(len(bot.conversations.get(key)), 4)
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
