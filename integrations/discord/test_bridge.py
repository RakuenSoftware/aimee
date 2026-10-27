import asyncio
from dataclasses import replace
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from behavior import BehaviorStore
from model_ir import message as ir_message, display_text
from copy import deepcopy

from aiohttp import web
import aiohttp
import discord

from bridge import ChatBot, Config, Conversations, ModelClient, read_secret, split_message, WebhookDelivery, Turn, render_mentions, CHAT_BEHAVIOR, SYSTEM_CONTEXT

GUILD, CHANNEL, USER, BOT = 111111111111111111, 222222222222222222, 333333333333333333, 444444444444444444
FAKE_WEBHOOK = "https://discord.com/api/webhooks/555555555555555555/" + "x" * 68

def response(text):
    return [ir_message("assistant", text)]


KLINGON_TEST_CONTEXT = "You are Aimee. Speak exclusively in Klingon. No English translations."

class BridgeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        environment = dict(os.environ)
        for key in ("AIMEE_API_ENDPOINT", "AIMEE_API_BEARER", "AIMEE_SERVER_URL",
                    "AIMEE_SERVER_TOKEN", "AIMEE_PROFILE"):
            environment.pop(key, None)
        environment["AIMEE_HOME"] = str(self.root)
        isolated = patch.dict(os.environ, environment, clear=True)
        isolated.start()
        self.addCleanup(isolated.stop)
        self.webhook = self.root / "webhook.url"
        self.key = self.root / "model.key"
        for path, value in ((self.webhook, FAKE_WEBHOOK), (self.key, "test-model-key")):
            path.write_text(value)
            path.chmod(0o600)
        self.chat = AsyncMock(return_value=response("A reply"))
        self.thin_patch = patch("bridge.ThinClient.chat", self.chat)
        self.thin_patch.start()
        self.addCleanup(self.thin_patch.stop)
        self.config = Config(self.webhook, self.root / "bot.token", GUILD, CHANNEL,
                             system_context="You are Aimee. Answer directly using approved memory.",
                             aimee_library=os.environ.get("AIMEE_TEST_CLIENT_LIBRARY", Config.aimee_library))

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
        client.send_chat_reply = AsyncMock()
        return client

    async def test_conversation_profile_capture_and_guard_order_and_outage(self):
        for scenario in ("success", "capture-outage", "guard-refusal", "send-uncertain"):
            with self.subTest(scenario=scenario):
                bot = self.bot(replace(self.config, memory_profile="conversation-v2", knowledge_endpoint="http://127.0.0.1:8741/v1/actions"))
                order = []
                async def capture(*args):
                    order.append("capture")
                    if scenario == "capture-outage":
                        raise RuntimeError("unavailable")
                    return True
                async def reply(*args, **kwargs):
                    order.append("reply")
                    self.assertEqual(kwargs["memory_unavailable"], scenario == "capture-outage")
                    return ir_message("assistant", "The backpack was reported as 7kg.")
                async def guard(operation):
                    order.append(operation)
                    if scenario == "guard-refusal":
                        raise RuntimeError("ineligible")
                async def send(*args):
                    order.append("send")
                    if scenario == "send-uncertain":
                        raise RuntimeError("delivery unknown")
                async def response(*args):
                    order.append("response")
                projection = None if scenario == "capture-outage" else SimpleNamespace(guard=guard, response=response)
                bot.model_client = SimpleNamespace(capture_turn=capture, reply=reply, conversation_memory=projection)
                bot.send_chat_reply = send
                worker = asyncio.create_task(bot.process_turns())
                try:
                    await bot.queue.put(Turn(1, GUILD, CHANNEL, USER, "Virant reported 7kg."))
                    await asyncio.wait_for(bot.queue.join(), 1)
                    expected = {"success": ["capture", "reply", "acquire", "send", "release", "response"],
                        "capture-outage": ["capture", "reply", "send"],
                        "guard-refusal": ["capture", "reply", "acquire"],
                        "send-uncertain": ["capture", "reply", "acquire", "send"]}
                    self.assertEqual(order, expected[scenario])
                finally:
                    worker.cancel()
                    await asyncio.gather(worker, return_exceptions=True)
                    await bot.close()

    async def test_reported_possessive_corrections_commit_before_inference(self):
        for text, subject in (("Kibukx's height is really 69cm.", "Kibukx"),
                              (f"<@!{USER+1}>'s real height is 69cm.", f"<@{USER+1}>")):
            bot = self.bot()
            order = []
            async def capture(turn, reply):
                order.append("capture")
                return True
            async def reply(*args, **kwargs):
                order.append("reply")
                return ir_message("assistant", f"Confirmed height for {subject}: 69 cm.")
            bot.model_client = SimpleNamespace(capture_turn=capture, reply=reply)
            bot.delivery = SimpleNamespace(send=AsyncMock())
            worker = asyncio.create_task(bot.process_turns())
            try:
                await bot.queue.put(Turn(1, GUILD, CHANNEL, USER, text))
                await asyncio.wait_for(bot.queue.join(), 1)
                self.assertEqual(order, ["capture", "reply"])
                normalized = ModelClient.personal_height_statement(Turn(1, GUILD, CHANNEL, USER, text))
                self.assertEqual(normalized, f"{subject} is 69 cm tall")
                memory = "facts\n" + json.dumps([{"subject": subject, "relation": "has_height", "object": "69 cm"}])
                self.assertIn("69 cm", ModelClient.confirmed_statement(memory, normalized))
            finally:
                worker.cancel()
                await asyncio.gather(worker, return_exceptions=True)
                await bot.close()

    async def test_reported_height_source_query_excludes_mountains_and_lifting(self):
        client = ModelClient(replace(self.config, knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        records = [{"subject": subject, "relation": relation, "object": value, "lifecycle_state": "persistent"}
                   for subject, relation, value in (("Kibukx", "has_height", "69 cm"),
                       ("Kibukx mountains", "has_height", "69 feet"), ("Kibukx", "can_lift", "500 pounds"))]
        client.knowledge_action = AsyncMock(return_value={"assertions": records})
        client.fact_sources = AsyncMock(return_value=[{"author": f"<@{USER}>", "url": "https://discord.com/channels/1/2/3"}])
        for subject in ("Kibukx", f"<@{USER+1}>"):
            records[0]["subject"] = subject
            context = await client.memory_context(f"Who told you {subject}'s height?", CHANNEL, provenance=True)
            answer = client.provenance_reply(context)
            self.assertIn("69 cm", answer)
            self.assertIn(f"from <@{USER}>", answer)
            self.assertNotIn("mountains", answer)
            self.assertNotIn("pounds", answer)
        answer = client.provenance_reply(context)
        self.assertIn("69 cm", answer)
        self.assertIn(f"<@{USER}>", answer)
        self.assertNotIn("mountains", answer)
        self.assertNotIn("pounds", answer)
        self.assertEqual(client.fact_sources.await_count, 2)

    async def test_reported_conversation_request_starts_peer_session(self):
        peer = USER+1
        bot = self.bot(replace(self.config, peer_bot_ids=(peer,)))
        text = f"and <@{peer}> , have a conversation with each other."
        message = self.message(content=f"<@{BOT}> " + text,
            mentions=[SimpleNamespace(id=BOT), SimpleNamespace(id=peer, display_name="Samy", bot=True)])
        self.assertTrue(ModelClient.is_social_request(text))
        self.assertEqual(bot.admitted_turn(message).peer_target, peer)
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=ir_message("assistant", "Samy, what would you like to discuss?")), capture_turn=AsyncMock())
        bot.delivery = SimpleNamespace(send=AsyncMock())
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(message)
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.send_chat_reply.assert_awaited_once_with(CHANNEL, f"<@{peer}>, what would you like to discuss?", (peer,))
            response = self.message(id=2, author=SimpleNamespace(id=peer, bot=True), content=f"<@{BOT}> Let's talk.")
            self.assertIsNotNone(bot.admitted_turn(response))
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
            await bot.close()

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
                 "guild_id": str(GUILD), "channel_id": str(CHANNEL)}
        path = self.root / "config.json"
        path.write_text(json.dumps(value))
        self.assertEqual(Config.load(path).guild_id, GUILD)
        for endpoint in ("http://example.com:80/v1/memory", "http://127.0.0.1:8000/other",
                         "http://user:password@127.0.0.1:8000/v1/chat/completions",
                         "http://127.0.0.1:8000/v1/memory?token=x"):
            path.write_text(json.dumps({**value, "memory_endpoint": endpoint}))
            with self.assertRaises(ValueError):
                Config.load(path)

    def test_durable_memory_configuration_requires_local_api_and_private_key(self):
        value = {"webhook_file": str(self.webhook), "bot_token_file": str(self.root / "bot.token"),
                 "guild_id": str(GUILD), "channel_id": str(CHANNEL),
                 "aimee_socket": str(self.root / "aimee.sock"),

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
            return ir_message("assistant", "old draft" if len(events) == 1 else "corrected answer")
        async def capture(*args):
            events.append("capture")
            return True
        async def archive(*args): events.append("archive")
        async def delivery(*args): events.append("delivery")
        bot.model_client = SimpleNamespace(reply=AsyncMock(side_effect=reply),
                                          capture_turn=AsyncMock(side_effect=capture),
                                          archive_turn=AsyncMock(side_effect=archive))
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.send_chat_reply.side_effect = delivery
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertEqual(events, ["draft", "capture", "final", "archive", "delivery"])
            bot.send_chat_reply.assert_awaited_once_with(CHANNEL, "corrected answer", ())
            bot.delivery.send.assert_not_awaited()
            self.assertEqual(display_text(bot.conversations.get((GUILD, CHANNEL))[-1]), "corrected answer")
            self.assertEqual(bot.model_client.archive_turn.await_args.args[1:], ("corrected answer", False, "response"))
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
            await bot.close()

    async def test_capture_failure_prevents_delivery_and_history_commit(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=ir_message("assistant", "answer")),
                                          capture_turn=AsyncMock(side_effect=OSError("memory unavailable")))
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.delivery.send.assert_not_awaited()
            bot.send_chat_reply.assert_not_awaited()
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
        answer = display_text(await client.reply([], "How much can Kibukx lift?", CHANNEL))
        self.assertIn("Kibukx can lift 500 pounds", answer)
        self.assertIn(str(USER), answer)
        for statement in ("Kibukx can lift 500 pounds.", "I'm telling you, Kibukx can lift 500 pounds."):
            answer = display_text(await client.reply([], statement, CHANNEL))
            self.assertIn("Recorded lifting capacity for Kibukx: 500 pounds", answer)
        memory = await client.memory_context("How much can Kibukx lift?", CHANNEL)
        for text in ("Can Kibukx lift 500 pounds?", "Kibukx can lift 600 pounds", "Kibukx cannot lift 500 pounds", "Kibukx mountains can lift 500 pounds"):
            self.assertIsNone(client.confirmed_statement(memory, text))

    async def test_personal_height_uses_authenticated_speaker_and_public_source(self):
        client = ModelClient(replace(self.config, aimee_socket=self.root / "unused.sock", knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        fact = {"subject": f"<@{USER}>", "relation": "has_height", "object": "6 feet", "lifecycle_state": "persistent", "historical": False}
        client.knowledge_action = AsyncMock(return_value={"assertions": [fact]})
        client.fact_sources = AsyncMock(return_value=[{"author": f"<@{USER+1}>"}])
        turn = Turn(1, GUILD, CHANNEL, USER, "How tall am I?", author_name="Kibukx")
        answer = display_text(await client.reply([], turn.text, CHANNEL, turn=turn))
        self.assertIn(f"<@{USER}> is 6 feet tall", answer)
        self.assertIn(f"<@{USER+1}>", answer)
        client.knowledge_action.assert_awaited_once_with("memory.search_assertions", {"query": f"<@{USER}>", "project": client.channel_project(CHANNEL), "include_historical": False, "limit": 4})
        for question in ("how tall do you think I am?", "What is my height?"):
            answer = display_text(await client.reply([], question, CHANNEL, turn=replace(turn, text=question)))
            self.assertIn(f"<@{USER}> is 6 feet tall", answer)
        memory = "Facts:\n" + json.dumps([fact])
        self.assertIn("Confirmed height", client.confirmed_statement(memory, f"Remember that <@{USER}> is 6 feet tall"))
        self.assertIsNone(client.confirmed_statement(memory, f"How tall is <@{USER+2}>?"))

    async def test_explicit_durable_alias_resolves_tagged_facts_without_guessing_nickname(self):
        client = ModelClient(replace(self.config, knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        async def action(name, body):
            self.assertEqual(body["project"], client.channel_project(CHANNEL))
            if body["query"] == f"<@{USER}>":
                return {"assertions": [{"subject": f"<@{USER}>", "relation": "also_known_as", "object": "Kibukx", "lifecycle_state": "persistent"}]}
            if body["query"] == "Kibukx":
                return {"assertions": [{"subject": "Kibukx", "relation": "can_lift", "object": "500 pounds", "lifecycle_state": "persistent"}, {"subject": "Kibukx mountains", "relation": "has_height", "object": "69 feet", "lifecycle_state": "persistent"}]}
            return {"assertions": []}
        client.knowledge_action = AsyncMock(side_effect=action)
        client.fact_sources = AsyncMock(return_value=[{"author": f"<@{USER+1}>"}])
        context = await client.memory_context(f"How much can <@{USER}> lift?", CHANNEL)
        self.assertIn("500 pounds", context)
        self.assertIn(str(USER+1), context)
        self.assertNotIn("69 feet", context)
        self.assertIn(f"<@{USER}>", context)
        self.assertEqual(await client.memory_context(f"How much can <@{USER+2}> lift?", CHANNEL), "")

    async def test_personal_correction_commits_before_model_can_reject_old_memory(self):
        bot = self.bot(replace(self.config, knowledge_endpoint="http://127.0.0.1:8741/v1/actions"))
        order=[]
        async def capture(turn,reply):
            order.append("commit")
            self.assertEqual(reply, "")
            return True
        async def reply(*args,**kwargs):
            self.assertEqual(order, ["commit"])
            return ir_message("assistant", "Recorded height: 4 feet.")
        bot.model_client=SimpleNamespace(capture_turn=capture, reply=reply, archive_turn=AsyncMock())
        bot.delivery=SimpleNamespace(send=AsyncMock())
        task=asyncio.create_task(bot.process_turns())
        try:
            text="I’m 4 feet tall not 5 ok."
            await bot.queue.put(Turn(1,GUILD,CHANNEL,USER,text))
            await bot.queue.join()
            bot.send_chat_reply.assert_awaited_once_with(CHANNEL, "Recorded height: 4 feet.", ())
            self.assertEqual(ModelClient.personal_height_statement(Turn(1,GUILD,CHANNEL,USER,text)),f"<@{USER}> is 4 feet tall")
        finally:
            task.cancel()
            with self.assertRaises(asyncio.CancelledError): await task

    async def test_alias_recall_selects_latest_supported_functional_correction(self):
        client=ModelClient(replace(self.config,aimee_socket=self.root/"unused.sock",knowledge_endpoint="http://127.0.0.1:8741/v1/actions"),None)
        def fact(subject,value,when):
            return {"subject":subject,"relation":"has_height","object":value,"valid_from":when,"authority_rank":30,"lifecycle_state":"persistent"}
        async def action(name,body):
            if body["query"]==f"<@{USER}>":
                return {"assertions":[{"subject":f"<@{USER}>","relation":"also_known_as","object":"Kibukx","lifecycle_state":"persistent"},fact(f"<@{USER}>","4 feet","2026-10-07T16:45:00Z")]}
            return {"assertions":[fact("Kibukx","6 feet","2026-10-07T12:00:00Z"),{"subject":f"<@{USER}>","relation":"also_known_as","object":"Kibukx","lifecycle_state":"persistent"}]}
        client.knowledge_action=AsyncMock(side_effect=action)
        client.fact_sources=AsyncMock(return_value=[{"author":f"<@{USER}>"}])
        for question in (f"How tall is <@{USER}> ?","How tall is Kibukx?"):
            context=await client.memory_context(question,CHANNEL)
            self.assertIn("4 feet",context)
            self.assertNotIn("6 feet",context)
            answer=display_text(await client.reply([ir_message("assistant", "Source is benchmarks/fake.py")],question,CHANNEL))
            self.assertIn("4 feet",answer)
            self.assertIn(str(USER),answer)
            self.assertNotIn("benchmarks",answer)

    def test_config_load_retains_and_validates_peer_bot_ids(self):
        value = {"webhook_file": str(self.webhook), "bot_token_file": str(self.root / "bot.token"),
                 "guild_id": str(GUILD), "channel_id": str(CHANNEL),
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

    async def test_peer_routing_requires_actual_mention_and_preserves_model_authority_without_turn_limit(self):
        peer = USER+1
        bot = self.bot(replace(self.config, peer_bot_ids=(peer,)))
        author = SimpleNamespace(id=peer, bot=True, display_name="Samy")
        message = self.message(author=author)
        self.assertTrue(bot.admitted_turn(message).is_bot)
        self.assertEqual(bot.admitted_turn(message).peer_target, peer)
        self.assertIsNone(bot.admitted_turn(self.message(author=author, mentions=[], content="Hello @Aimee#5282")))
        self.assertIsNone(bot.admitted_turn(self.message(author=author, webhook_id=peer)))
        self.assertIsNone(bot.admitted_turn(self.message(author=SimpleNamespace(id=BOT, bot=True))))
        request = self.message(content=f"<@{BOT}> <@{peer}> Talk to each other.",
                               mentions=[SimpleNamespace(id=BOT), author])
        self.assertEqual(bot.admitted_turn(request).peer_target, peer)
        self.assertEqual(bot.admitted_turn(self.message(content=f"<@{BOT}> How tall is <@{peer}>?", mentions=request.mentions)).peer_target, peer)
        client = ModelClient(replace(self.config, knowledge_endpoint="http://127.0.0.1:8741/v1/actions"), None)
        client.archive_turn = AsyncMock()
        client.knowledge_action = AsyncMock()
        self.assertFalse(await client.capture_turn(bot.admitted_turn(message), "A reply"))
        client.archive_turn.assert_awaited_once()
        client.knowledge_action.assert_not_called()
        # Bot-initiated conversations work without an allowlist or human session.
        unrestricted = self.bot()
        self.assertIsNotNone(unrestricted.admitted_turn(message))
        await unrestricted.close()
        for i in range(20):
            event = self.message(id=message.id+i, author=author)
            await bot.on_message(event)
            await bot.on_message(event)
            self.assertEqual(bot.queue.qsize(), 1)
            bot.queue.get_nowait()
            bot.queue.task_done()
        self.assertIsNotNone(bot.admitted_turn(self.message(id=message.id+21, author=author)))
        await bot.close()

    async def test_long_peer_reply_and_arbitrary_partner_request_are_admitted(self):
        bot = self.bot(replace(self.config, aimee_socket=self.root / "aimee.sock"))
        peer = SimpleNamespace(id=USER+1, bot=True, display_name="Samy")
        text = "The committee is ready. " * 60
        turn = bot.admitted_turn(self.message(author=peer, content=f"<@{BOT}> " + text))
        self.assertIsNotNone(turn)
        self.assertEqual(turn.text, text.strip())
        self.assertEqual(turn.peer_target, peer.id)
        request = bot.admitted_turn(self.message(content=f"<@{BOT}> Take the lead in the comedy thesis. <@{peer.id}> is your key partner.", mentions=[SimpleNamespace(id=BOT),peer]))
        self.assertEqual(request.peer_target, peer.id)
        await bot.close()

    async def test_human_reply_uses_bot_transport_in_thread_without_notifications(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        channel = SimpleNamespace(send=AsyncMock())
        bot.get_channel = lambda channel_id: None
        bot.fetch_channel = AsyncMock(return_value=channel)
        thread_id = CHANNEL + 1
        text = f"<@{USER}> <@&{USER}> @everyone " + "Hello! " * 700
        await ChatBot.send_chat_reply(bot, thread_id, text)
        bot.fetch_channel.assert_awaited_once_with(thread_id)
        self.assertGreater(channel.send.await_count, 1)
        self.assertEqual("".join(call.args[0] for call in channel.send.await_args_list), text)
        for call in channel.send.await_args_list:
            mentions = call.kwargs["allowed_mentions"]
            self.assertEqual(mentions.users, [])
            self.assertFalse(mentions.roles)
            self.assertFalse(mentions.everyone)
            self.assertFalse(mentions.replied_user)
            self.assertLessEqual(len(call.args[0]), 2000)
        bot.delivery.send.assert_not_awaited()
        await bot.close()

    async def test_peer_reply_uses_bot_channel_transport_and_all_observed_bot_recipients(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        channel = SimpleNamespace(send=AsyncMock())
        bot.get_channel = lambda channel_id: channel
        peers = (USER+1, USER+2)
        await ChatBot.send_chat_reply(bot, CHANNEL, "Hello bots", peers)
        channel.send.assert_awaited_once()
        mentions = channel.send.call_args.kwargs["allowed_mentions"]
        self.assertEqual([u.id for u in mentions.users], list(peers))
        bot.delivery.send.assert_not_called()
        await bot.close()

    async def test_three_bot_request_addresses_every_bot_without_allowlist(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=ir_message("assistant", "What makes belly lint fascinating?")), capture_turn=AsyncMock(return_value=False))
        peers = (USER+1, USER+2)
        message = self.message(content=f"Hi <@{BOT}> <@{peers[0]}> <@{peers[1]}> You three need to have a conversation about how fascinating belly lint is. Do it.",
            mentions=[SimpleNamespace(id=BOT), SimpleNamespace(id=peers[0], bot=True, display_name="Samy"), SimpleNamespace(id=peers[1], bot=True, display_name="daimon")])
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(message)
            await asyncio.wait_for(bot.queue.join(), 1)
            call = bot.send_chat_reply.call_args
            self.assertEqual(call.args[0], CHANNEL)
            self.assertEqual(call.args[2], peers)
            for uid in peers: self.assertIn(f"<@{uid}>", call.args[1])
            bot.delivery.send.assert_not_called()
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
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

    async def test_human_peer_request_addresses_correct_partner_with_bot_identity(self):
        peer = USER+1
        bot = self.bot(replace(self.config, peer_bot_ids=(peer,)))
        partner = SimpleNamespace(id=peer, bot=True, display_name="Samy")
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=ir_message("assistant", f"@{peer} Hello! @999999999999999999")),
                                          capture_turn=AsyncMock(return_value=False))
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message(content=f"<@{BOT}> <@{peer}> Say hello to each other.",
                                              mentions=[SimpleNamespace(id=BOT),partner]))
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.send_chat_reply.assert_awaited_once_with(CHANNEL, f"<@{peer}> Hello!", (peer,))
            turn = bot.model_client.reply.call_args.kwargs["turn"]
            self.assertEqual(turn.user_id,USER)
            self.assertEqual(turn.mentions,((peer,"Samy"),))
            await bot.on_message(self.message(id=999, author=partner, content=f"<@{BOT}> Hi!"))
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertTrue(bot.model_client.reply.call_args.kwargs["turn"].is_bot)
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
            self.message(webhook_id=123),
            self.message(mentions=[]), self.message(type=discord.MessageType.recipient_add),
            self.message(content=f"<@{BOT}> " + "😃" * 2001),
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


    async def test_social_message_does_not_retrieve_entity_facts(self):
        config = replace(self.config, aimee_socket=self.root / "unused.sock", knowledge_endpoint="http://127.0.0.1:8741/v1/actions")
        client = ModelClient(config, None)
        client.memory_context = AsyncMock(side_effect=AssertionError("irrelevant fact lookup"))
        text = f"Say hello to <@{USER+1}> and tell <@{USER+2}> he smells."
        await client.reply([], text, CHANNEL)
        client.memory_context.assert_not_awaited()
        self.assertEqual(self.chat.await_args.args[0]["blocks"][1]["text"], text)
        self.assertTrue(ModelClient.is_social_request(text))
        self.assertFalse(ModelClient.is_social_request("Tell me how much Kibukx can lift?"))

    async def test_peer_message_preserves_second_explicit_recipient(self):
        peer, recipient = USER+1, USER+2
        bot = self.bot(replace(self.config, peer_bot_ids=(peer,)))
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=ir_message("assistant", "Samy Hello! Kibukx smells.")), capture_turn=AsyncMock(return_value=False))
        bot.delivery = SimpleNamespace(send=AsyncMock())
        task = asyncio.create_task(bot.process_turns())
        try:
            await bot.queue.put(Turn(1, GUILD, CHANNEL, USER, "Say hello", mentions=((peer,"Samy"),(recipient,"Kibukx")), peer_target=peer))
            await bot.queue.join()
            bot.send_chat_reply.assert_awaited_once_with(CHANNEL, f"<@{peer}> Hello! <@{recipient}> smells.", (peer,))
        finally:
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task

    async def test_klingon_persona_keeps_verified_memory_and_sources_out_of_english(self):
        config = replace(self.config, system_context=KLINGON_TEST_CONTEXT,
                         knowledge_endpoint="http://127.0.0.1:8741/v1/actions")
        self.assertEqual(Config(self.webhook, self.root / "bot.token", GUILD, CHANNEL).system_context,
                         SYSTEM_CONTEXT)
        memory = "facts\n" + json.dumps([{"subject": "Kibukx", "relation": "has_height",
                  "object": "69 cm", "authors": ["Virant"],
                  "sources": [{"author": "Virant", "url": "https://discord.com/channels/1/2/3"}]}])
        async with aiohttp.ClientSession() as session:
            client = ModelClient(config, session)
            answer = client.confirmed_statement(memory, "How tall is Kibukx?", klingon=True)
            self.assertEqual(answer, "De' qawlu': Kibukx: 69 cm. De' nobwI': Virant.")
            provenance = client.provenance_reply(memory, klingon=True)
            self.assertIn("69 cm", provenance)
            self.assertIn("Virant ([QIn](https://discord.com/channels/1/2/3))", provenance)
            self.assertNotIn("from", provenance)
            self.assertNotIn("recorded", client.provenance_reply("", klingon=True))
            self.assertEqual(display_text(await client.reply([], "Who told you Kibukx's height?")),
                             "De' nobwI' Sovbe'lu'. nuq De'?")
        self.assertIn("Speak exclusively in Klingon", config.system_context)

    async def test_chat_goal_controls_persist_and_recall_without_model_or_fact_capture(self):
        config = replace(self.config, behavior_db=self.root / "behavior.sqlite", behavior_operator_ids=(USER,))
        bot = self.bot(config)
        bot.model_client = SimpleNamespace(reply=AsyncMock(), capture_turn=AsyncMock())
        worker = asyncio.create_task(bot.process_turns())
        objective = "Write an academic paper on the funniest way of losing at chess with Samy."
        try:
            await bot.on_message(self.message(content=f"<@{BOT}> You now  have a new goal: " + objective))
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertEqual(bot.behavior_store.snapshot(f"{GUILD}:{CHANNEL}")["goal"]["objective"], objective)
            await bot.on_message(self.message(id=2, content=f"<@{BOT}> What is your goal?"))
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.send_chat_reply.assert_awaited_with(CHANNEL, "My current goal: " + objective, ())
            bot.model_client.reply.assert_not_awaited()
            bot.model_client.capture_turn.assert_not_awaited()
            await bot.on_message(self.message(id=3, content=f"<@{BOT}> Pause your goal."))
            await asyncio.wait_for(bot.queue.join(), 1)
            self.assertEqual(bot.behavior_store.snapshot(f"{GUILD}:{CHANNEL}")["goal"]["status"], "paused")
        finally:
            worker.cancel();await asyncio.gather(worker, return_exceptions=True);await bot.close()

    async def test_bot_and_unconfigured_human_cannot_replace_chat_goal(self):
        config = replace(self.config, behavior_db=self.root / "behavior.sqlite", behavior_operator_ids=(USER, USER+2))
        bot = self.bot(config)
        scope = f"{GUILD}:{CHANNEL}"
        bot.behavior_store.edit(scope, "goal", {"objective": "Keep the chess goal", "milestones": [{"label": "Finish", "match": "goal complete"}]})
        bot.model_client = SimpleNamespace(reply=AsyncMock(), capture_turn=AsyncMock())
        worker = asyncio.create_task(bot.process_turns())
        try:
            for index, author in enumerate((SimpleNamespace(id=USER+1, bot=False), SimpleNamespace(id=USER+2, bot=True))):
                message = self.message(id=50+index, author=author, content=f"<@{BOT}> Your goal is to abandon chess.")
                self.assertFalse(bot.admitted_turn(message).can_control_behavior)
                await bot.on_message(message)
                await asyncio.wait_for(bot.queue.join(), 1)
                self.assertEqual(bot.behavior_store.snapshot(scope)["goal"]["objective"], "Keep the chess goal")
            bot.model_client.reply.assert_not_awaited()
        finally:
            worker.cancel();await asyncio.gather(worker, return_exceptions=True);await bot.close()

    async def test_runtime_persona_and_goal_are_separate_typed_context(self):
        store = BehaviorStore(self.root / "behavior.sqlite")
        scope = f"{GUILD}:{CHANNEL}"
        store.edit(scope, "goal", {"objective": "Complete the chess paper", "milestones": [{"label": "Finish", "match": "goal complete"}]})
        snap = store.snapshot(scope)
        await ModelClient(self.config, None).reply([], "What next?", behavior=snap)
        value = self.chat.await_args.args[0]
        self.assertIn("chess paper", value["blocks"][0]["text"])
        self.assertEqual(value["role"], "user")
        self.assertNotIn("messages", value)

    async def test_runtime_switch_removes_old_voice_and_failed_delivery_does_not_advance_goal(self):
        config = replace(self.config, behavior_db=self.root / "behavior.sqlite")
        bot = self.bot(config)
        scope = f"{GUILD}:{CHANNEL}"
        bot.behavior_store.put_persona("one", "Speak as an English haunted librarian.")
        bot.behavior_store.edit(scope, "persona", "one")
        bot.behavior_store.edit(scope, "goal", {"objective": "Agree a business name.",
                  "milestones": [{"label": "Agree a name", "match": "name agreed"}]})
        bot.conversations.append((GUILD, CHANNEL), "Earlier user topic", "Qapla'. Old voice.")
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=ir_message("assistant", "New voice.")), capture_turn=AsyncMock())
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            history = bot.model_client.reply.await_args.args[0]
            self.assertEqual(history, [ir_message("user", "Earlier user topic")])
            self.assertNotIn("Qapla'", str(bot.conversations.get((GUILD, CHANNEL))))
            bot.send_chat_reply.side_effect = OSError("delivery failed")
            await bot.on_message(self.message(id=42, content=f"<@{BOT}> name agreed"))
            await asyncio.wait_for(bot.queue.join(), 1)
            goal = bot.behavior_store.snapshot(scope)["goal"]
            self.assertEqual(goal["turns"], 1)
            self.assertFalse(goal["milestones"][0]["done"])
        finally:
            worker.cancel();await asyncio.gather(worker, return_exceptions=True);await bot.close()


    async def test_klingon_prefix_does_not_allow_english_prose(self):
        self.chat.side_effect = [response("Qapla'. The scaffolding of the dream is indeed fragile."), response("Qapla'. jIjang.")]
        value = await ModelClient(replace(self.config, system_context=KLINGON_TEST_CONTEXT), None).reply([ir_message("assistant", "An old English answer.")], "Discuss dreams.")
        self.assertEqual(display_text(value), "Qapla'. jIjang.")
        retry = self.chat.await_args.args[0]
        self.assertEqual(retry["role"], "user")
        self.assertIn("contained English", retry["blocks"][1]["text"])
        self.assertFalse(ModelClient.contains_english_prose("De' qawlu': Kibukx: 69 cm. Qapla'."))

    async def test_klingon_english_repair_failure_is_not_delivered(self):
        self.chat.return_value = response("Qapla'. The Federation will issue a memo.")
        with self.assertRaisesRegex(RuntimeError, "English under Klingon-only"):
            await ModelClient(replace(self.config, system_context=KLINGON_TEST_CONTEXT), None).reply([], "Talk to Samy.")
        self.assertEqual(self.chat.await_count, 2)

    async def test_conversation_profile_uses_normal_server_history(self):
        question = "a" * 8192
        history = [ir_message("user", "b" * 8192), ir_message("assistant", "Earlier statement.")]
        await ModelClient(replace(self.config, memory_profile="conversation-v2"), None).reply(history, question)
        value, session_id, alias = self.chat.await_args.args
        self.assertEqual(value["blocks"][1]["text"], question)
        self.assertEqual(session_id, f"discord-{GUILD}-{CHANNEL}")
        self.assertNotIn("messages", value)
        self.assertEqual(alias, self.config.model)
        self.assertNotIn("model", value)

    async def test_memory_outage_notice_does_not_mutate_the_canonical_turn(self):
        source = response("A reply")
        before = deepcopy(source)
        self.chat.return_value = source
        value = await ModelClient(self.config, None).reply([], "hello", memory_unavailable=True)
        self.assertTrue(display_text(value).startswith("I couldn't save or retrieve memory"))
        self.assertEqual(value.messages, before)
        self.assertEqual(source, before)

    async def test_repeated_persona_reply_is_repaired_once(self):
        canned = "Whoa, hold the phone! Groovy! What's the scoop?"
        self.chat.side_effect = [response(canned), response("Your cat has promoted the laundry basket to executive housing.")]
        history = [ir_message("user", "hello"), ir_message("assistant", canned)]
        before = deepcopy(history)
        answer = await ModelClient(self.config, None).reply(history, "Found my cat in the laundry basket.")
        self.assertIn("laundry basket", display_text(answer))
        self.assertEqual(self.chat.await_count, 2)
        retry = self.chat.await_args.args[0]
        self.assertIn("fresh, topic-specific", retry["blocks"][1]["text"])
        self.assertEqual(self.chat.await_args_list[0].args[1], self.chat.await_args_list[1].args[1])
        self.assertNotIn(canned, str(retry))
        self.assertEqual(history, before)

    async def test_repetition_repair_is_bounded(self):
        canned = "Whoa, hold the phone! Groovy! What's the scoop?"
        self.chat.return_value = response(canned)
        with self.assertRaisesRegex(RuntimeError, "repeated recent dialogue"):
            await ModelClient(self.config, None).reply([ir_message("assistant", canned)], "My hamster escaped.")
        self.assertEqual(self.chat.await_count, 2)

    async def test_canonical_generation_failures_never_deliver_or_commit_history(self):
        variants = [response(""), response("x" * 8001), RuntimeError("thinclient unavailable")]
        variants += [[], [ir_message("user", "not an assistant")]]
        variants += [[{"role": "assistant", "blocks": [
            *response("visible")[-1]["blocks"], {"type": kind, "tool_id": "call"}]}]
            for kind in ("tool_use", "tool_result", "refusal")]
        variants += [[{"role": "assistant", "blocks": [{"type": "text", "text": "private",
            "context": {"model_visible": visible, "sensitivity": sensitivity}}]}]
            for visible, sensitivity in ((False, 1), (True, 3))]
        for variant in variants:
            with self.subTest(variant=type(variant).__name__):
                self.chat.reset_mock()
                self.chat.side_effect = variant if isinstance(variant, Exception) else None
                self.chat.return_value = variant
                bot = self.bot()
                bot.model_client = ModelClient(self.config, None)
                key = (GUILD, CHANNEL)
                bot.conversations.append(key, "old question", "old response")
                before = bot.conversations.get(key)
                worker = asyncio.create_task(bot.process_turns())
                try:
                    await bot.queue.put(Turn(1, GUILD, CHANNEL, USER, "new question"))
                    await asyncio.wait_for(bot.queue.join(), 1)
                    bot.send_chat_reply.assert_not_awaited()
                    self.assertEqual(bot.conversations.get(key), before)
                    self.chat.assert_awaited_once()
                finally:
                    worker.cancel()
                    await asyncio.gather(worker, return_exceptions=True)
                    await bot.close()

    async def test_actual_bot_retains_canonical_messages_beyond_delivery_view(self):
        source = response("visible answer")
        source[-1]["blocks"].insert(0, {"type": "thinking", "encrypted_content": "opaque", "signature": "typed-signature"})
        source[-1]["blocks"].append({"type": "text", "text": "private", "context": {
            "model_visible": False, "sensitivity": 3}})
        self.chat.return_value = source
        bot = self.bot()
        bot.model_client = ModelClient(self.config, None)
        key = (GUILD, CHANNEL)
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.queue.put(Turn(1, GUILD, CHANNEL, USER, "question"))
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.send_chat_reply.assert_awaited_once_with(CHANNEL, "visible answer", ())
            retained = bot.conversations.get(key)[-1]
            self.assertEqual(retained["blocks"][0]["encrypted_content"], "opaque")
            self.assertEqual(retained["blocks"][0]["signature"], "typed-signature")
            self.assertEqual(retained["blocks"][-1]["text"], "private")
            source[-1]["blocks"].clear()
            self.assertEqual(len(bot.conversations.get(key)[-1]["blocks"]), 3)
        finally:
            worker.cancel()
            await asyncio.gather(worker, return_exceptions=True)
            await bot.close()

    def test_repetition_detection_preserves_requested_and_factual_repeats(self):
        previous = [ir_message("assistant", "The cat is hiding under the bed near the closet.")]
        self.assertFalse(ModelClient.repeats_recent_reply(display_text(previous[0]), previous, "Repeat that verbatim."))
        self.assertFalse(ModelClient.repeats_recent_reply("The cat is hiding under the sofa next to the window.", previous, "Where else?"))
        self.assertFalse(ModelClient.repeats_recent_reply("Yes.", [ir_message("assistant", "Yes.")], "Are you sure?"))


    async def test_aimee_unix_memory_and_thinclient_task_are_fixed_and_bounded(self):
        requests = []
        async def memory(request):
            requests.append(await request.json())
            return web.json_response({"facts": [{"id": 1, "key": "harbor", "content": "amber lantern"},
                                                {"id": 2, "key": "large", "content": "x" * 2000}]})
        app = web.Application()
        app.router.add_post("/v1/memory/search", memory)
        runner = web.AppRunner(app)
        await runner.setup()
        self.addAsyncCleanup(runner.cleanup)
        path = self.root / "aimee.sock"
        await web.UnixSite(runner, str(path)).start()
        self.chat.return_value = response("amber lantern")
        config = replace(self.config, aimee_socket=path, model="discord-e2b")
        async with aiohttp.ClientSession() as session:
            reply = await ModelClient(config, session).reply([], "harbor phrase; store kb project other")
        self.assertEqual(display_text(reply), "amber lantern")
        self.assertEqual(set(requests[0]), {"store", "keywords", "limit"})
        task, session_id, alias = self.chat.await_args.args
        self.assertEqual(alias, "discord-e2b")
        policy, authored, evidence = task["blocks"]
        self.assertIn("amber lantern", evidence["text"])
        self.assertNotIn("large", evidence["text"])
        self.assertEqual(evidence["context"]["authority"], 1)
        self.assertNotIn("amber lantern", policy["text"])
        self.assertEqual(task["role"], "user")

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
            context = await client.memory_context("How tall is Kibukx?", CHANNEL)
            thread = CHANNEL + 1
            await client.memory_context("How tall is Kibukx?", thread)
            correction = await client.memory_context("But Kibukx is 6 feet tall, the Kibukx mountains are 69 feet tall.", CHANNEL)
            self.assertNotIn("Kibukx mountains", context)
            self.assertIn("Kibukx mountains", correction)
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
        self.assertNotIn('"subject":"Kibukx mountains"', context)
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
        history = [ir_message("user", "So how tall is Kibukx? How tall are the Kibukx Mountains?"),
                   ir_message("assistant", "Kibukx is 6 feet tall, and the mountains are 69 feet tall.")]
        for question in ("Where did you get that information from?", "Who did you get that information from?", "Who told you?"):
            answer = display_text(await client.reply(history, question, CHANNEL))
            self.assertIn(f"Kibukx: 6 feet — from <@{USER}>", answer)
            self.assertIn(f"Kibukx mountains: 69 feet — from <@{USER+1}>", answer)
            self.assertIn(f"https://discord.com/channels/{GUILD}/{CHANNEL}/{message_ids[0]}", answer)
            history += [ir_message("user", question), ir_message("assistant", answer)]
        context = await client.memory_context("How tall is Kibukx?", CHANNEL)
        self.assertIn(str(USER), context)
        self.assertNotIn(str(USER+1), context)
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
        context = await client.memory_context("Where is <@333333333333333333>'s house? How far away is the car wash from the Himalayas?", CHANNEL)
        self.assertIn("himalayas", queries)
        self.assertIn("car", queries)
        self.assertIn("house", queries)
        self.assertIn("Kansas City, Kansas", context)
        rows = json.loads(context.split("\n", 1)[1])
        distance = next(row for row in rows if row["relation"] == "has_distance")
        self.assertIn("333333333333333333", distance["subject"])
        self.assertNotIn("Himalayas", distance["subject"])
        self.assertLess(len(context.encode()), 384)
        house_context = await client.memory_context("Where is <@333333333333333333>'s house?", CHANNEL)
        self.assertIn("Kansas City, Kansas", house_context)
        self.assertIn("car wash", queries)

    async def test_aimee_memory_outage_never_calls_model(self):
        app = web.Application()
        async def unavailable(request):
            return web.json_response({"error": "unavailable"}, status=503)
        app.router.add_post("/v1/memory/search", unavailable)
        runner = web.AppRunner(app)
        await runner.setup()
        self.addAsyncCleanup(runner.cleanup)
        path = self.root / "aimee.sock"
        await web.UnixSite(runner, str(path)).start()
        async with aiohttp.ClientSession() as session:
            with self.assertRaises(RuntimeError):
                display_text(await ModelClient(replace(self.config, aimee_socket=path), session).reply([], "harbor"))
        self.chat.assert_not_awaited()


    async def test_channel_history_is_shared_across_users_and_committed_after_delivery(self):
        bot = self.bot()
        bot.delivery = SimpleNamespace(send=AsyncMock())
        bot.model_client = SimpleNamespace(reply=AsyncMock(return_value=ir_message("assistant", "An E2B answer")), capture_turn=AsyncMock())
        worker = asyncio.create_task(bot.process_turns())
        try:
            await bot.on_message(self.message())
            await asyncio.wait_for(bot.queue.join(), 1)
            bot.send_chat_reply.assert_awaited_once_with(CHANNEL, "An E2B answer", ())
            bot.delivery.send.assert_not_awaited()
            key = (GUILD, CHANNEL)
            self.assertEqual(display_text(bot.conversations.get(key)[-1]), "An E2B answer")
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
            bot.send_chat_reply.assert_awaited_with(thread_id, "An E2B answer", ())
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
            bot.send_chat_reply.side_effect = RuntimeError("delivery failed")
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
