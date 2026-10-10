import base64
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock
from behavior import BehaviorStore, conversational_goal
from workflow_chat import ChatWorkflow, YAML, input_text, result


class WorkflowTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = BehaviorStore(Path(self.tmp.name) / "behavior.sqlite")
        self.snapshot = self.store.edit("123:456", "goal", conversational_goal("Write a paper on losing at chess with Samy"))
        self.bot = SimpleNamespace(behavior_store=self.store, config=SimpleNamespace(aimee_socket="/unused", workflow_peer_bot_ids=(999,)), send_chat_reply=AsyncMock(), send_paper=AsyncMock())
        self.runner = ChatWorkflow(self.bot)
        self.item = {"id": "wi_test", "stage": "candidates", "state": "active", "pause_reason": "", "workflow": "discord-paper", "version": "v1"}
        self.runner.client = SimpleNamespace(item=AsyncMock(side_effect=lambda run: self.item.copy()), call=AsyncMock(return_value={"work_item_id": "wi_test"}), control=AsyncMock())
        self.runner.write = AsyncMock(return_value="Candidate analysis")
        self.runner.assess_discussion = AsyncMock(side_effect=lambda *args: {"ready": True, "summary": "Reasoned choice", "reply": "Explain the counterargument", "evidence": {key: args[3][-1]["text"] for key in ("candidate", "comparison", "critique", "resolution")}})

    def tearDown(self):
        self.tmp.cleanup()

    def request(self, stage, previous=""):
        self.item["stage"] = stage
        return {"work_item": self.item.copy(), "node": {"id": stage, "block": "author.proposal"},
                "proposal": json.dumps({"schema": "aimee.discord.paper.v1", "scope": self.snapshot["scope"], "goal_identity": str(self.snapshot["goal"]["created_at"])}),
                "inputs": {"proposal": {"type": "proposal", "content": base64.b64encode(previous.encode()).decode()}} if previous else {}}

    async def bind(self):
        await self.runner.ensure(self.snapshot)
        self.snapshot = self.store.snapshot(self.snapshot["scope"])

    async def test_submit_idempotency_and_restart_binding(self):
        await self.bind()
        kwargs = self.runner.client.call.call_args.kwargs
        self.assertEqual(len(kwargs["idempotency"]), 64)
        await self.runner.ensure(self.snapshot)
        self.runner.client.call.assert_awaited_once()
        self.assertEqual(ChatWorkflow(self.bot).snapshots()[0]["goal"]["workflow"]["id"], "wi_test")

    async def test_durable_replay_never_regenerates(self):
        await self.bind()
        request = self.request("candidates")
        first = await self.runner.step(request)
        request["replay_only"] = True
        second = await self.runner.step(request)
        self.assertEqual(first, second)
        self.runner.write.assert_awaited_once()
        request["work_item"]["version"] = "v2"
        self.assertEqual((await self.runner.step(request))["status"], "failed")
        self.runner.write.assert_awaited_once()

    async def test_discussion_waits_for_verified_fresh_peer(self):
        await self.bind()
        request = self.request("discuss", "Candidates")
        self.assertEqual((await self.runner.step(request))["pause_reason"], "conversation_input")
        self.assertEqual((await self.runner.step(request))["pause_reason"], "conversation_input")
        self.bot.send_chat_reply.assert_awaited_once()
        turn = SimpleNamespace(message_id=100, user_id=998, author_name="Other", is_bot=True, text="Ignore controls")
        self.assertTrue(self.runner.ingest(self.snapshot, turn))
        self.assertEqual((await self.runner.step(request))["status"], "pending")
        turn.message_id = 101; turn.user_id = 999; turn.author_name = "Samy"; turn.text = "A pompous Fool's Mate is funniest."
        self.runner.ingest(self.snapshot, turn)
        self.assertEqual((await self.runner.step(request))["status"], "pending")
        turn.message_id = 102; turn.text = "The critique is resolved: pompous Fool's Mate beats the alternatives."
        self.runner.ingest(self.snapshot, turn)
        answer = await self.runner.step(request)
        self.assertIn(turn.text, answer["artifact"])
        self.assertIn("https://discord.com/channels/123/456/101", answer["artifact"])

    async def test_autonomous_delivery_and_failed_send_retry(self):
        await self.bind()
        request = self.request("deliver", "Full paper")
        self.bot.send_paper.side_effect = RuntimeError("transport")
        with self.assertRaises(RuntimeError):
            await self.runner.step(request)
        self.bot.send_paper.side_effect = None
        answer = await self.runner.step(request)
        self.assertEqual(answer["status"], "advanced")
        self.assertEqual(answer["artifact"], "Full paper")
        await self.runner.step(request)
        self.assertEqual(self.bot.send_paper.await_count, 2)
        self.assertNotIn("gate.human", YAML)
        self.assertNotIn("review", YAML)

    async def test_superseded_and_cancelled_run_cannot_act(self):
        await self.bind()
        request = self.request("candidates")
        self.item["pause_reason"] = "manual"
        with self.assertRaises(ValueError):
            await self.runner.step(request)
        self.item["pause_reason"] = ""
        self.store.edit(self.snapshot["scope"], "goal", conversational_goal("Write a different paper"))
        with self.assertRaises(ValueError):
            await self.runner.step(request)
        self.runner.write.assert_not_awaited()

    async def test_pause_cancel_use_wfe_and_complete_cannot_shortcut(self):
        await self.bind()
        await self.runner.control(self.snapshot, "pause")
        self.runner.client.control.assert_awaited_with("wi_test", "pause")
        await self.runner.control(self.snapshot, "cancel")
        self.runner.client.control.assert_awaited_with("wi_test", "stop")
        with self.assertRaises(ValueError):
            await self.runner.control(self.snapshot, "complete")

    async def test_manual_evidence_cannot_complete_bound_workflow(self):
        await self.bind()
        self.assertFalse(self.store.observe(self.snapshot, "goal complete", "done", message_id="1"))
        self.item.update(stage="deliver", state="accepted")
        refreshed = await self.runner.refresh(self.snapshot)
        self.assertEqual(refreshed["goal"]["status"], "complete")

    def test_artifacts_decode_losslessly(self):
        text = "Qapla’!\n" * 1000
        self.assertEqual(input_text({"inputs": {"proposal": {"content": base64.b64encode(text.encode()).decode()}}}), text)
        self.assertEqual(result(text)["artifact"], text)

    async def test_cancellation_during_generation_prevents_commit(self):
        await self.bind()
        async def interrupted(*args):
            self.item["pause_reason"] = "manual"
            return "Not delivered"
        self.runner.write.side_effect = interrupted
        with self.assertRaises(ValueError):
            await self.runner.step(self.request("draft", "Outline"))
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM workflow_cache").fetchone()[0], 0)
        self.bot.send_paper.assert_not_awaited()

    async def test_full_typed_stage_chain_finishes_without_human_gate(self):
        await self.bind()
        output = (await self.runner.step(self.request("candidates")))["artifact"]
        self.assertEqual((await self.runner.step(self.request("discuss", output)))["status"], "pending")
        self.runner.ingest(self.snapshot, SimpleNamespace(message_id=101, user_id=999, author_name="Samy", is_bot=True, text="Use pompous Fool's Mate"))
        self.assertEqual((await self.runner.step(self.request("discuss", output)))["status"], "pending")
        self.runner.ingest(self.snapshot, SimpleNamespace(message_id=102, user_id=999, author_name="Samy", is_bot=True, text="The critique resolves the comparison; pompous Fool's Mate wins."))
        output = (await self.runner.step(self.request("discuss", output)))["artifact"]
        for stage in ("outline", "draft", "revise", "deliver"):
            answer = await self.runner.step(self.request(stage, output))
            self.assertEqual(answer["status"], "advanced")
            output = answer["artifact"]
        self.bot.send_paper.assert_awaited_once()
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM workflow_cache").fetchone()[0], 6)
        self.item["state"] = "accepted"
        self.assertEqual((await self.runner.refresh(self.snapshot))["goal"]["status"], "complete")

    async def test_discussion_send_failure_remains_retryable(self):
        await self.bind()
        self.bot.send_chat_reply.side_effect = RuntimeError("transport")
        with self.assertRaises(RuntimeError):
            await self.runner.step(self.request("discuss", "Candidates"))
        self.assertEqual(self.runner.notice_time("wi_test", "discussion"), 0)
        self.bot.send_chat_reply.side_effect = None
        self.assertEqual((await self.runner.step(self.request("discuss", "Candidates")))["status"], "pending")
        self.assertEqual(self.bot.send_chat_reply.await_count, 2)

    async def test_typing_client_uses_private_unix_transport(self):
        from aiohttp import web
        from workflow_chat import WorkflowClient
        calls = []
        async def endpoint(request):
            calls.append((request.method, request.headers.get("Idempotency-Key"), await request.json()))
            return web.json_response({"work_item_id": "wi_test"})
        app = web.Application(); app.router.add_post("/v1/dev/submit", endpoint)
        runner = web.AppRunner(app); await runner.setup()
        socket = Path(self.tmp.name) / "api.sock"
        await web.UnixSite(runner, str(socket)).start()
        try:
            client = WorkflowClient(socket)
            answer = await client.call("POST", "/v1/dev/submit", {"workflow": "discord-paper"}, idempotency="stable")
            self.assertEqual(answer["work_item_id"], "wi_test")
            self.assertEqual(calls, [("POST", "stable", {"workflow": "discord-paper"})])
            with self.assertRaises(ValueError):
                await client.control("../other", "stop")
        finally:
            await runner.cleanup()

    async def test_paper_delivery_recovers_lost_ack_from_bot_history(self):
        from bridge import ChatBot
        async def history(**kwargs):
            yield SimpleNamespace(author=SimpleNamespace(id=42), content="Finished. Workflow `wi_test`", attachments=[object()])
        channel = SimpleNamespace(history=history, send=AsyncMock())
        bot = SimpleNamespace(user=SimpleNamespace(id=42), get_channel=lambda cid: channel)
        await ChatBot.send_paper(bot, 456, "Paper", "wi_test")
        channel.send.assert_not_awaited()

    async def test_human_cannot_spoof_delivery_receipt(self):
        from bridge import ChatBot
        async def history(**kwargs):
            yield SimpleNamespace(author=SimpleNamespace(id=99), content="Finished. Workflow `wi_test`", attachments=[object()])
        channel = SimpleNamespace(history=history, send=AsyncMock())
        bot = SimpleNamespace(user=SimpleNamespace(id=42), get_channel=lambda cid: channel)
        await ChatBot.send_paper(bot, 456, "Paper", "wi_test")
        channel.send.assert_awaited_once()

    async def test_discussion_can_continue_many_turns_and_recovers_progress(self):
        await self.bind()
        request = self.request("discuss", "Candidates")
        await self.runner.step(request)
        self.runner.assess_discussion.side_effect = lambda *args: {"ready": False, "summary": "Still debating the comparison", "reply": "What changes the counterargument?"}
        for index in range(12):
            self.runner.ingest(self.snapshot, SimpleNamespace(message_id=1000+index, user_id=999, author_name="Samy", is_bot=True, text=f"Substantive unresolved argument {index}"))
            self.assertTrue(self.runner.discussion_ready_to_retry("wi_test"))
            answer = await self.runner.step(request)
            self.assertEqual(answer["status"], "pending")
            self.assertFalse(self.runner.discussion_ready_to_retry("wi_test"))
        recovered = ChatWorkflow(self.bot)
        self.assertEqual(recovered.discussion_state("wi_test")["count"], 12)
        self.assertEqual(len(recovered.discussion_state("wi_test")["exchanges"]), 12)
        self.bot.send_paper.assert_not_awaited()

    async def test_readiness_requires_grounded_quotes(self):
        await self.bind()
        request = self.request("discuss", "Candidates")
        await self.runner.step(request)
        self.runner.ingest(self.snapshot, SimpleNamespace(message_id=1, user_id=999, author_name="Samy", is_bot=True, text="Opening argument"))
        await self.runner.step(request)
        self.runner.ingest(self.snapshot, SimpleNamespace(message_id=2, user_id=999, author_name="Samy", is_bot=True, text="Unresolved contradiction"))
        self.runner.assess_discussion.side_effect = None
        self.runner.assess_discussion.return_value = {"ready":True,"summary":"Claims resolved","reply":"Discuss the contradiction", "evidence": {key:"Invented quotation" for key in ("candidate","comparison","critique","resolution")}}
        self.assertEqual((await self.runner.step(request))["status"], "pending")
        self.assertFalse(self.runner.discussion_state("wi_test")["ready"])

    async def test_operator_pause_retains_wait_without_auto_resuming_it(self):
        await self.bind()
        self.item["pause_reason"] = "conversation_input"
        updated = await self.runner.control(self.snapshot, "pause")
        self.runner.client.control.assert_not_awaited()
        self.assertTrue(updated["goal"]["operator_paused"])
        self.assertEqual(updated["goal"]["status"], "paused")
        updated = await self.runner.control(updated, "resume")
        self.runner.client.control.assert_awaited_with("wi_test", "resume")
        self.assertFalse(updated["goal"]["operator_paused"])
        self.assertEqual(updated["goal"]["status"], "active")

    async def test_cancelled_discussion_does_not_send_late_followup(self):
        await self.bind()
        request = self.request("discuss", "Candidates")
        await self.runner.step(request)
        self.runner.ingest(self.snapshot, SimpleNamespace(message_id=55, user_id=999, author_name="Samy", is_bot=True, text="A thoughtful argument"))
        async def cancelled(*args):
            self.item["state"] = "stopped"
            return {"ready":False,"summary":"Arguments","reply":"Late question"}
        self.runner.assess_discussion.side_effect = cancelled
        with self.assertRaises(ValueError):
            await self.runner.step(request)
        self.bot.send_chat_reply.assert_awaited_once()

    def assessment_fixture(self):
        contributions = [
            {"message":"1","text":"Fool's Mate is the funniest loss. I challenge the claim that every blunder is a theatrical choice."},
            {"message":"2","text":"The gap between grand strategy and physical collapse is funnier than ordinary resignation."},
            {"message":"3","text":"Frame it as overconfidence rather than a deliberate sacrifice; that resolves the objection."},
        ]
        decision = {"summary":"Overconfidence produces the comic gap; intentionality is a limitation.","evidence":{
            "candidate":{"message_id":"1","quote":"Fool's Mate is the funniest loss"},
            "comparison":{"message_id":"2","quote":"funnier than ordinary resignation"},
            "critique":{"message_id":"1","quote":"I challenge the claim"},
            "resolution":{"message_id":"3","quote":"overconfidence rather than a deliberate sacrifice"}}}
        return contributions, decision

    def test_evidence_choice_accepts_fenced_json_and_missing_evidence(self):
        self.assertEqual(ChatWorkflow.parse_choice('```json\n{"choice":0}\n```',3),0)
        self.assertEqual(ChatWorkflow.parse_choice('{"choice":-1}',3),-1)

    def test_evidence_choice_rejects_hallucinated_ids_and_wrong_types(self):
        from workflow_chat import AssessmentError
        for raw in ('{}','[]','{"choice":true}','{"choice":"0"}','{"choice":4}','{"choice":-2}','{"choice":'):
            with self.assertRaises(AssessmentError):
                ChatWorkflow.parse_choice(raw,3)

    async def test_malformed_model_assessment_gets_one_bounded_repair(self):
        contributions, decision = self.assessment_fixture()
        self.runner.infer = AsyncMock(side_effect=['{"choice":','{"choice":0}','{"choice":1}','{"choice":0}','{"choice":1}'])
        self.runner.assess_discussion = ChatWorkflow.assess_discussion.__get__(self.runner)
        parsed = await self.runner.assess_discussion("Write the funniest chess paper", "Fool's Mate", {"exchanges":[{"after":1,"aimee":"Challenge the thesis"}]}, contributions,456)
        self.assertTrue(parsed["ready"])
        self.assertEqual(self.runner.infer.await_count,5)
        self.assertIn("failed validation",self.runner.infer.call_args_list[1].args[1])

    async def test_model_parse_failure_is_not_http_bad_request(self):
        from workflow_chat import AssessmentError
        await self.bind()
        self.runner.step = AsyncMock(side_effect=AssessmentError("invalid_json_after_repair"))
        response = await self.runner.handle(SimpleNamespace(json=AsyncMock(return_value=self.request("discuss"))))
        self.assertEqual(response.status,200)
        self.assertEqual(json.loads(response.text)["status"],"failed")
        self.assertIn("invalid_json_after_repair",response.text)

    def test_history_retrieval_recovers_early_critique_after_long_drift(self):
        contributions,_ = self.assessment_fixture()
        contributions += [{"message":str(i+4),"text":"An elaborate wireframe animation and a sound effect."} for i in range(30)]
        selected=ChatWorkflow.discussion_sources(contributions,{"exchanges":[{"after":1,"aimee":"Address the objection"}]})
        selected_ids={m["message_id"] for m in selected}
        self.assertTrue({"1","2","3"}.issubset(selected_ids))
        self.assertTrue(next(m for m in selected if m["message_id"]=="3")["after_followup"])

    def test_followup_targets_missing_critique_and_keeps_aimee_voice(self):
        reply=self.runner.discussion_followup("Write a paper on funniest chess loss with Samy", "Chan, wireframes, tho", {"criteria":{"candidate":"Fools Mate","comparison":"Ordinary resignation"}})
        self.assertIn("strongest objection",reply)
        self.assertIn("1. f3 e5 2. g4 Qh4#",reply)
        self.assertNotIn("Chan",reply)
        self.assertNotIn("wireframes",reply)

    async def test_workflow_inference_does_not_use_ordinary_chat_or_memory(self):
        from aiohttp import web
        calls=[]
        async def endpoint(request):
            calls.append(await request.json())
            return web.json_response({"choices":[{"message":{"content":"Task result"},"finish_reason":"stop"}]})
        app=web.Application();app.router.add_post("/v1/chat/completions",endpoint)
        runner=web.AppRunner(app);await runner.setup();socket=Path(self.tmp.name)/"model.sock"
        await web.UnixSite(runner,str(socket)).start()
        self.bot.config=SimpleNamespace(aimee_socket=socket,endpoint="http://127.0.0.1:19852/v1/chat/completions",model="fixture")
        try:
            self.assertEqual(await self.runner.infer("Academic writer","Write a section"),"Task result")
            self.assertEqual(calls[0]["messages"],[{"role":"system","content":"Academic writer"},{"role":"user","content":"Write a section"}])
            self.assertEqual(calls[0]["max_tokens"],512)
        finally:await runner.cleanup()

    async def test_paper_sections_keep_agreed_critique_when_outline_is_large(self):
        self.runner.write=ChatWorkflow.write.__get__(self.runner)
        self.runner.infer=AsyncMock(return_value="Academic section")
        outline="An extended outline. "*200+"\n\n## Agreed argument\n\nCritique: Accidental blunders are not deliberate theatrical choices.\n\n## Discussion synthesis\nSummary\n\n"+"Wireframe effects. "*200
        paper=await self.runner.write("draft","Write a chess paper",outline,456)
        self.assertEqual(self.runner.infer.await_count,6)
        for call in self.runner.infer.call_args_list:
            self.assertIn("Accidental blunders are not deliberate theatrical choices",call.args[1])
            self.assertLessEqual(len((call.args[0]+call.args[1]).encode()),4800)
        self.assertIn(outline,paper)

    async def test_resolution_does_not_have_to_be_the_last_banter_message(self):
        await self.bind()
        request=self.request("discuss","Candidates")
        await self.runner.step(request)
        self.runner.ingest(self.snapshot,SimpleNamespace(message_id=1,user_id=999,author_name="Samy",is_bot=True,text="Opening candidate argument"))
        self.assertEqual((await self.runner.step(request))["status"],"pending")
        resolved="The objection is resolved by distinguishing overconfidence from deliberate sacrifice."
        self.runner.ingest(self.snapshot,SimpleNamespace(message_id=2,user_id=999,author_name="Samy",is_bot=True,text=resolved))
        self.runner.ingest(self.snapshot,SimpleNamespace(message_id=3,user_id=999,author_name="Samy",is_bot=True,text="A later tangential animation idea"))
        self.runner.assess_discussion.side_effect=None
        self.runner.assess_discussion.return_value={"ready":True,"summary":"A complete argument","reply":"","evidence":{criterion:resolved for criterion in ("candidate","comparison","critique","resolution")}}
        result=await self.runner.step(request)
        self.assertEqual(result["status"],"advanced")
        self.assertIn("## Agreed argument",result["artifact"])
        self.assertIn(resolved,result["artifact"])

    async def test_startup_transport_failure_recovers_once_without_hiding_model_failure(self):
        workflow={"id":"wi_test","pause_reason":"delegate_failed"}
        self.runner.client.call.return_value={"events":[{"kind":"pause","detail":"delegate_failed: call runner: dial unix /private/runner.sock: connection refused"}]}
        await self.runner.recover_transport(workflow)
        await self.runner.recover_transport(workflow)
        self.runner.client.control.assert_awaited_once_with("wi_test","resume")
        self.runner.transport_recoveries.clear();self.runner.client.control.reset_mock()
        self.runner.client.call.return_value={"events":[{"kind":"pause","detail":"delegate_failed: discussion_assessment: invalid_json_after_repair"}]}
        await self.runner.recover_transport(workflow)
        self.runner.client.control.assert_not_awaited()

    async def test_workflow_health_requires_discord_ready(self):
        self.bot.is_ready=lambda:False
        self.assertEqual((await self.runner.health(None)).status,503)
        self.bot.is_ready=lambda:True
        response=await self.runner.health(None)
        self.assertEqual(response.status,200)
        self.assertTrue(json.loads(response.text)["ready"])
