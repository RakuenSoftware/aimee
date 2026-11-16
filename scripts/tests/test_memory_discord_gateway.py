"""Exercise the maintained canonical Discord delivery boundary without services.

The old lab gateway was retired with the concrete memory owner. Public Discord
integration consumes canonical Aimee Messages through the shared thinclient;
answer text is a delivery view, never a replacement for retained Messages.
"""
from copy import deepcopy
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("model_ir", ROOT / "integrations/discord/model_ir.py")
model_ir = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model_ir)


class GatewayTests(unittest.TestCase):
    def test_delivery_projects_answer_and_retains_complete_canonical_messages(self):
        source = [
            {"role": "assistant", "blocks": [{"type": "tool_use", "name": "read",
                "tool_id": "source-1", "input": {"id": 9007199254740993}}]},
            {"role": "tool", "blocks": [{"type": "tool_result", "tool_id": "source-1",
                "content": "source evidence"}]},
            {"role": "assistant", "blocks": [
                {"type": "thinking", "encrypted_content": "opaque"},
                {"type": "text", "text": "answer 古"},
                {"type": "text", "text": "private", "context": {
                    "model_visible": False, "sensitivity": 3}}]},
        ]
        original = deepcopy(source)
        turn = model_ir.chat_turn(source)
        self.assertEqual(model_ir.display_text(turn), "answer 古")
        self.assertEqual(turn.messages, original)
        self.assertEqual(turn.message, original[-1])
        source[0]["blocks"][0]["input"]["id"] = 0
        source[-1]["blocks"][1]["text"] = "mutated"
        self.assertEqual(turn.messages, original)
        self.assertEqual(model_ir.display_text(turn), "answer 古")

    def test_provider_wire_and_unfinished_tool_calls_are_not_canonical_answers(self):
        for value in (
            {"choices": [{"message": {"content": "provider answer"}}]},
            [{"role": "assistant", "content": "provider answer"}],
            [{"role": "assistant", "blocks": [{"type": "tool_use",
                "tool_id": "unexecuted", "name": "read", "input": {}}]}],
            [{"role": "tool", "blocks": [{"type": "tool_result",
                "tool_id": "source-1", "content": "not an answer"}]}],
        ):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                model_ir.chat_turn(value)

    def test_retired_lab_subprocess_gateway_is_not_restored(self):
        self.assertFalse((ROOT / "scripts/validation/memory/lxc/discord-gateway-request.py").exists())


if __name__ == "__main__":
    unittest.main()
