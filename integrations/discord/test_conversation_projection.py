import json
import unittest
from conversation_projection import annotations, rebuild


def episode(text, speaker="1", bot=False, timestamp=1000000):
    return {"source": "discord:1:2:3", "speaker_id": speaker, "speaker_name": "Virant",
        "speaker_kind": "bot" if bot else "human", "text": text, "observed_unix_ms": timestamp}


class ProjectionTests(unittest.IsolatedAsyncioTestCase):
    async def test_human_state_keeps_raw_units_and_finite_preference_lifetime(self):
        value = episode("Our goal is pack a backpack. I prefer brief replies for next 2 minutes. Keep 7kg, not pounds.")
        values = annotations(value)
        self.assertEqual({p["category"] for p in values}, {"purpose", "preferences", "constraints", "recent_human_context"})
        self.assertEqual(next(p for p in values if p["category"] == "preferences")["expires_at"], 1120)
        self.assertEqual(annotations(episode("We agreed on 69 feet.", bot=True)), [])
        self.assertEqual(annotations(episode("In our story, we agreed on 69 feet.")), [])

    async def test_rebuild_after_restart_revalidates_sources_and_expiry(self):
        source = episode("We agreed to use the red backpack. I prefer short replies for next 2 minutes.")
        pointers = []
        for item in annotations(source):
            item.update(schema_version=1, project="discord:1:2", source_id=10, source=source["source"])
            pointers.append({"key": "discord-projection:10:"+item["category"], "content": json.dumps(item)})
        deleted = False
        async def action(name, args):
            if deleted: return {"kind": "not_found"}
            self.assertEqual(args["project"], "discord:1:2")
            return {"memory": {"id": 10, "key": "discord:1:2:3", "content": json.dumps(source), "provenance_category": "user_stated"}}
        projection, records = await rebuild(action, "discord:1:2", pointers, now=1100)
        self.assertEqual(projection["decisions"][0]["source_id"], 10)
        self.assertEqual(len(projection["preferences"]), 1)
        projection, _ = await rebuild(action, "discord:1:2", pointers, now=1121)
        self.assertEqual(projection["preferences"], [])
        deleted = True
        projection, records = await rebuild(action, "discord:1:2", pointers, now=1100)
        self.assertEqual(records, [])
        self.assertEqual(projection["decisions"], [])

    async def test_private_scope_and_changed_speaker_are_refused(self):
        source = episode("We agreed to use a red backpack.")
        pointer = next(p for p in annotations(source) if p["category"] == "decisions")
        pointer.update(schema_version=1, project="discord:1:2", source_id=10, source=source["source"])
        async def action(name, args):
            return {"memory": {"id": 10, "key": "discord:private:2:3", "content": json.dumps(source), "provenance_category": "user_stated"}}
        with self.assertRaisesRegex(RuntimeError, "identity changed"):
            await rebuild(action, "discord:1:2", [{"key": "discord-projection:10", "content": json.dumps(pointer)}])

    async def test_forged_revocation_and_lifetime_cannot_control_projection(self):
        source = episode("I prefer brief replies for next 2 minutes.")
        pointer = next(p for p in annotations(source) if p["category"] == "preferences")
        pointer.update(schema_version=1, project="discord:1:2", source_id=10, source=source["source"])
        async def action(name, args):
            return {"memory": {"id": 10, "key": source["source"], "content": json.dumps(source), "provenance_category": "user_stated"}}
        for change in ({"revoked": True}, {"expires_at": 1000000}):
            altered = {**pointer, **change}
            with self.assertRaisesRegex(RuntimeError, "differs from its source"):
                await rebuild(action, "discord:1:2", [{"key": "discord-projection:10", "content": json.dumps(altered)}], now=1050)

    async def test_feedback_is_provenance_bound_and_bot_agreement_is_not_a_decision(self):
        self.assertEqual(annotations(episode("Your answer was wrong; correction: the person is 69cm tall."))[0]["speaker_id"], "1")
        self.assertEqual(annotations(episode("We agreed that Virant is correct.", bot=True)), [])
