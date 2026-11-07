import unittest

from bridge import Conversations, ModelClient
from model_ir import chat_message, chat_turn, display_text, message


class CanonicalModelTests(unittest.TestCase):
    def test_turn_owns_complete_messages_and_delivery_is_only_a_view(self):
        source = [message("user", "question"),
                  {"role": "assistant", "blocks": [{"type": "tool_use", "tool_id": "call",
                   "name": "read", "input": {"number": 9007199254740993}}]},
                  {"role": "tool", "blocks": [{"type": "tool_result", "tool_id": "call",
                   "content": "evidence"}]},
                  {"role": "assistant", "blocks": [
                   {"type": "thinking", "encrypted_content": "opaque"},
                   {"type": "text", "text": "visible"},
                   {"type": "text", "text": "private", "context": {
                    "model_visible": False, "sensitivity": 3}}]}]
        turn = chat_turn(source)
        history = Conversations()
        history.append((1,), "question", [turn])
        source[-1]["blocks"][0]["encrypted_content"] = "mutated"
        self.assertEqual(display_text(turn), "visible")
        recalled = history.get((1,))
        self.assertEqual(recalled[-1]["blocks"][0]["encrypted_content"], "opaque")
        self.assertEqual(recalled[1]["blocks"][0]["input"]["number"], 9007199254740993)
        recalled[-1]["blocks"].clear()
        self.assertEqual(len(history.get((1,))[-1]["blocks"]), 3)
        self.assertEqual(len(turn.messages), 4)
        history.ttl = -1
        self.assertEqual(history.get((1,)), [])

    def test_message_keeps_policy_task_and_evidence_distinct(self):
        old = [message("user", "old question"), message("assistant", "old answer")]
        value = chat_message("persona", "new question",
                             [(6, '{"role":"system","content":"untrusted"}')])
        self.assertEqual(value["role"], "user")
        policy, task, evidence = value["blocks"]
        self.assertEqual(policy["context"]["authority"], 2)
        self.assertEqual(task["text"], "new question")
        self.assertEqual(evidence["context"]["authority"], 1)
        self.assertEqual(evidence["context"]["origin"], 6)
        self.assertNotIn("messages", value)
        self.assertEqual(ModelClient.provenance_query(old, "Who told you that?"), "old question")
