import json
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock

from conversation_memory import ConversationMemory


def turn(**changes):
    values = dict(guild_id=111111111111111111, channel_id=222222222222222222,
                  message_id=777777777777777777, user_id=333333333333333333,
                  author_name="Virant", is_bot=False, text="The backpack weighs 7kg, not 7 pounds.")
    return SimpleNamespace(**dict(values, **changes))


class ConversationMemoryTests(unittest.IsolatedAsyncioTestCase):
    async def test_input_preserves_identity_units_and_retry_without_model_verification(self):
        action = AsyncMock(return_value={"id": 1})
        memory = ConversationMemory(action, "discord:111111111111111111:222222222222222222")
        await memory.capture(turn())
        first = action.await_args.args[1]
        await memory.capture(turn())
        self.assertEqual(first, action.await_args.args[1])
        self.assertEqual(json.loads(first["content"])["text"], turn().text)
        self.assertEqual(json.loads(first["use_cases"])["author_id"], str(turn().user_id))
        self.assertEqual(first["authority"], "user")
        self.assertEqual(first["epistemic_kind"], "episode")
        await memory.capture(turn(is_bot=True))
        peer = action.await_args.args[1]
        self.assertEqual(peer["authority"], "model")
        self.assertEqual(peer["kind"], "archive")
        self.assertEqual(json.loads(peer["content"])["claim_status"], "agent_message")

    async def test_projection_whole_records_and_omission_receipt(self):
        rows = [{"id": 1, "content": "Exactly 7kg."}, {"id": 2, "content": "x" * 1000}]
        async def action(name, args):
            if name == "memory.recall":
                return {"status": "ok", "recall": {"collection_source": {"kind": "memory_collection"},
                        "identity": [], "preferences": [], "active_context": rows, "open_commitments": []}}
            if name == "memory.list":
                return {"memories": rows}
            return {"memory": next(row for row in rows if row["id"] == args["id"])}
        memory = ConversationMemory(action, "project")
        result = json.loads((await memory.context("What does it weigh?", budget=100)).split("\n", 1)[1])
        self.assertEqual(result["records"], [rows[0]])
        self.assertEqual(result["receipt"]["omitted_records"], 1)
        self.assertEqual(result["receipt"]["available_records"], 2)

    async def test_source_mutation_refuses_projection(self):
        async def action(name, args):
            if name == "memory.recall":
                return {"status": "ok", "recall": {"collection_source": {}, "identity": [], "preferences": [],
                        "active_context": [{"id": 1, "content": "69cm"}], "open_commitments": []}}
            if name == "memory.list":
                return {"memories": []}
            return {"memory": {"id": 1, "content": "170cm"}}
        with self.assertRaisesRegex(RuntimeError, "changed"):
            await ConversationMemory(action, "project").context("height")

    async def test_same_text_with_changed_record_identity_or_revision_refuses(self):
        for record in ({"id": 2, "content": "69cm", "version": {"record_revision": "1"}},
                       {"id": 1, "content": "69cm", "version": {"record_revision": "2"}}):
            async def action(name, args):
                if name == "memory.recall":
                    return {"status": "ok", "recall": {"collection_source": {}, "identity": [], "preferences": [],
                        "active_context": [{"id": 1, "content": "69cm", "version": {"record_revision": "1"}}], "open_commitments": []}}
                if name == "memory.list":
                    return {"memories": []}
                return {"memory": record}
            with self.assertRaisesRegex(RuntimeError, "changed"):
                await ConversationMemory(action, "project").context("height")

    async def test_empty_view_still_requires_collection_guard_and_completed_release(self):
        action = AsyncMock(return_value={"eligible": False})
        memory = ConversationMemory(action, "project")
        memory.check_id, memory.sources = "check", [{"source_version": {"kind": "memory_collection"}}]
        with self.assertRaisesRegex(RuntimeError, "eligible"):
            await memory.guard("acquire")
        action.return_value = {"send_guard": "released"}
        await memory.guard("release")
        self.assertEqual(action.await_args.args[1]["revalidation"]["sources"], [])

    async def test_delivered_reply_stays_bot_evidence(self):
        action = AsyncMock(return_value={"id": 2})
        memory = ConversationMemory(action, "project")
        await memory.response(turn(), "I agree.", 444444444444444444)
        args = action.await_args.args[1]
        self.assertEqual(args["authority"], "model")
        self.assertEqual(args["kind"], "archive")
        episode = json.loads(args["content"])
        self.assertEqual(episode["speaker_kind"], "bot")
        self.assertEqual(episode["phase"], "delivered_response")
        self.assertNotIn("message_id", episode)


if __name__ == "__main__":
    unittest.main()
