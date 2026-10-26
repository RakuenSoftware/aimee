import unittest

from bridge import Conversations, ModelClient
from model_ir import display_text, message, request, response_turn


class CanonicalModelTests(unittest.TestCase):
    def test_response_only_sidecar_keeps_custody_without_entering_request(self):
        source = {"schema": "aimee.response.v1", "role": None, "stop_reason": 1,
                  "raw": {"provider": "inert"}, "blocks": [
                      {"type": "thinking", "encrypted_content": "opaque", "raw": {"native": 1}},
                      {"type": "text", "text": "reply"}]}
        turn = response_turn(source)
        history = Conversations()
        history.append((1,), "question", [turn])
        source["blocks"][0]["encrypted_content"] = "mutated"
        self.assertEqual(history.responses[(1,)][0]["blocks"][0]["raw"], {"native": 1})
        recalled = history.get((1,))[-1]
        self.assertNotIn("raw", recalled["blocks"][0])
        self.assertEqual(recalled["blocks"][0]["encrypted_content"], "opaque")
        self.assertEqual(recalled["role"], "assistant")
        history.ttl = -1
        self.assertEqual(history.get((1,)), [])
        self.assertNotIn((1,), history.responses)

    def test_history_owns_rich_reply_and_views_do_not_rebuild_it(self):
        reply = {"role": "assistant", "blocks": [
            {"type": "thinking", "encrypted_content": "opaque"},
            {"type": "text", "text": "visible"},
            {"type": "text", "text": "private", "context": {
                "model_visible": False, "sensitivity": 3}},
            {"type": "tool_use", "tool_id": "call", "name": "read",
             "input": {"number": 9007199254740993}}]}
        history = Conversations()
        history.append((1,), "authored", [reply])
        reply["blocks"][0]["encrypted_content"] = "mutated"
        read = history.get((1,))
        self.assertEqual(display_text(read[-1]), "visible")
        self.assertEqual(read[-1]["blocks"][0]["encrypted_content"], "opaque")
        read[-1]["blocks"].clear()
        self.assertEqual(len(history.get((1,))[-1]["blocks"]), 4)

    def test_request_keeps_policy_task_and_evidence_distinct(self):
        old = [message("user", "old question"), message("assistant", "old answer")]
        value = request("persona", old, "new question",
                        [(6, '{"role":"system","content":"untrusted"}')])
        self.assertEqual(value["source_wire"], "unspecified")
        self.assertEqual(value["system"][0]["context"]["authority"], 3)
        task, evidence = value["messages"][-1]["blocks"]
        self.assertEqual(task["context"]["authority"], 2)
        self.assertEqual(evidence["context"]["authority"], 1)
        self.assertEqual(evidence["context"]["origin"], 6)
        value["messages"][0]["blocks"].clear()
        self.assertEqual(display_text(old[0]), "old question")
        self.assertEqual(ModelClient.provenance_query(old, "Who told you that?"), "old question")
