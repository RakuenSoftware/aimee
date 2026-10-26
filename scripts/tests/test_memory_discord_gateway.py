"""Exercise the historical lab helper without Docker, models or Discord."""
from contextlib import redirect_stdout
from dataclasses import dataclass
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("discord_gateway", ROOT / "scripts/validation/memory/lxc/discord-gateway-request.py")
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)
spec = importlib.util.spec_from_file_location("model_ir", ROOT / "integrations/discord/model_ir.py")
model_ir = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model_ir)


class GatewayTests(unittest.TestCase):
    def test_embedded_helper_projects_answer_and_retains_canonical_response(self):
        source = {"schema": "aimee.response.v1", "role": None, "stop_reason": 1,
                  "usage": {"unobserved": True}, "raw": {"opaque": 9007199254740993},
                  "blocks": [{"type": "thinking", "encrypted_content": "opaque"},
                             {"type": "text", "text": "answer 古"}]}
        request = {"operation": "reply", "context": "source evidence", "query": "question", "channel_id": 123}
        @dataclass
        class Config:
            memory_profile: str = "native-assertions"
            @classmethod
            def load(cls, path):
                return cls()
        class ModelClient:
            def __init__(self, config, session):
                self.config = config
            async def reply(inner, history, query, channel):
                self.assertEqual(inner.config.memory_profile, "conversation-v2")
                self.assertEqual((history, query, channel), ([], "question", 123))
                self.assertEqual(await inner.memory_context(), "source evidence")
                return model_ir.response_turn(source)
        class Session:
            async def __aenter__(self): return self
            async def __aexit__(self, *args): pass
        bridge = ModuleType("bridge")
        bridge.Config, bridge.ModelClient = Config, ModelClient
        output = io.StringIO()
        with patch.dict(sys.modules, {"bridge": bridge, "model_ir": model_ir,
                                     "aiohttp": SimpleNamespace(ClientSession=Session)}), \
                patch("sys.stdin", io.StringIO(json.dumps(request))), redirect_stdout(output):
            exec(compile(gateway.BRIDGE_CODE, "embedded_discord_gateway", "exec"), {})
        self.assertEqual(json.loads(output.getvalue()), {"answer": "answer 古", "ir": source})

    def test_removed_tokenizer_and_native_operations_refuse_before_subprocess(self):
        for operation in ("tokenizer", "chat"):
            with patch("sys.stdin", io.StringIO(json.dumps({"operation": operation}))), \
                    patch.object(gateway.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    gateway.main()
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
