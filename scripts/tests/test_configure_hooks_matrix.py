#!/usr/bin/env python3
"""Golden compatibility matrix for configure-hooks.sh.

Each declared adapter must preserve unrelated configuration, install exactly one
managed entry, and produce byte-identical files on a second run.
"""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ConfigureHooksMatrixTest(unittest.TestCase):
    def test_supported_adapter_matrix_is_merge_safe_and_idempotent(self):
        with tempfile.TemporaryDirectory(prefix="aimee-hook-matrix-") as tmp:
            home = Path(tmp)
            adapters = {
                "claude": (home / ".claude/settings.json", None),
                "gemini": (home / ".gemini/settings.json", None),
                "codex": (home / ".codex/hooks.json", home / ".codex/mcp-config.json"),
                "copilot": (home / ".copilot/config.json", home / ".copilot/mcp-config.json"),
            }
            for name, (hooks_path, mcp_path) in adapters.items():
                hooks_path.parent.mkdir(parents=True, exist_ok=True)
                hooks_path.write_text(
                    json.dumps(
                        {
                            "unrelated": {"adapter": name, "keep": True},
                            "hooks": {
                                "UnrelatedEvent": [
                                    {
                                        "matcher": "*",
                                        "hooks": [{"type": "command", "command": "keep-me"}],
                                    }
                                ]
                            },
                        }
                    )
                )
                if mcp_path:
                    mcp_path.write_text(
                        json.dumps({"unrelated": name, "mcpServers": {"keep": {"command": "x"}}})
                    )

            vscode = home / ".config/Code/User/mcp.json"
            vscode.parent.mkdir(parents=True)
            vscode.write_text(json.dumps({"unrelated": "vscode", "servers": {"keep": {"command": "x"}}}))

            env = os.environ.copy()
            env.update({"HOME": str(home), "OSTYPE": "linux-gnu", "TERM": "dumb"})
            command = ["bash", str(ROOT / "configure-hooks.sh")]
            subprocess.run(command, env=env, cwd=ROOT, check=True, capture_output=True, text=True)

            managed_paths = [v for pair in adapters.values() for v in pair if v] + [vscode]
            first = {path: path.read_bytes() for path in managed_paths}
            subprocess.run(command, env=env, cwd=ROOT, check=True, capture_output=True, text=True)
            second = {path: path.read_bytes() for path in managed_paths}
            self.assertEqual(first, second, "a second installation must be byte-for-byte idempotent")

            for name, (hooks_path, mcp_path) in adapters.items():
                hooks = json.loads(hooks_path.read_text())
                self.assertEqual(hooks["unrelated"], {"adapter": name, "keep": True})
                self.assertIn("UnrelatedEvent", hooks["hooks"])
                managed_commands = [
                    hook["command"]
                    for entries in hooks["hooks"].values()
                    for entry in entries
                    for hook in entry.get("hooks", [])
                    if "aimee" in hook.get("command", "")
                ]
                self.assertGreaterEqual(len(managed_commands), 2)
                self.assertEqual(len(managed_commands), len(set(managed_commands)))
                mcp = json.loads((mcp_path or hooks_path).read_text())
                self.assertIn("aimee", mcp["mcpServers"])
                if mcp_path:
                    self.assertEqual(mcp["unrelated"], name)
                    self.assertIn("keep", mcp["mcpServers"])

            vscode_config = json.loads(vscode.read_text())
            self.assertEqual(vscode_config["unrelated"], "vscode")
            self.assertIn("keep", vscode_config["servers"])
            self.assertIn("aimee", vscode_config["servers"])


if __name__ == "__main__":
    unittest.main()
