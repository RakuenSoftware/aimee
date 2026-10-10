import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from bridge import SYSTEM_CONTEXT


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.model = self.root / "model"
        self.model.mkdir()
        (self.model / "config.json").write_text(json.dumps({"model_type": "gemma4", "text_config": {
            "hidden_size": 1536, "num_hidden_layers": 35}}))
        (self.model / "tokenizer.json").write_text("{}")
        self.gguf = self.root / "checkpoint.gguf"
        self.gguf.write_bytes(b"GGUFsynthetic test fixture, not model weights")
        self.output, self.key = self.root / "private", self.root / "private/model.key"

    def prepare(self):
        return subprocess.run([sys.executable, str(Path(__file__).with_name("prepare_e2b.py")),
            "--model", str(self.model), "--gguf", str(self.gguf), "--output", str(self.output),
            "--model-key", str(self.key)], capture_output=True, text=True)

    def test_private_config_binding_and_no_overwrite(self):
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        path = self.output / "serving.json"
        original = path.read_bytes()
        key = self.key.read_bytes()
        config = json.loads(original)
        self.assertEqual(config["adapter"], "gemma4-e2b")
        self.assertEqual(config["native_system_context"], SYSTEM_CONTEXT)
        self.assertEqual(config["serve"]["host"], "127.0.0.1")
        self.assertFalse(config["serve"]["enable_prefix_caching"])
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.key.stat().st_mode & 0o777, 0o600)
        self.assertNotEqual(self.prepare().returncode, 0)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(self.key.read_bytes(), key)

    def test_wrong_model_is_refused_before_writing_credentials(self):
        (self.model / "config.json").write_text(json.dumps({"model_type": "gemma4", "text_config": {
            "hidden_size": 2560, "num_hidden_layers": 42}}))
        self.assertNotEqual(self.prepare().returncode, 0)
        self.assertFalse(self.key.exists())

    def test_invalid_checkpoint_is_refused_before_writing_config(self):
        self.gguf.write_bytes(b"not a GGUF checkpoint")
        self.assertNotEqual(self.prepare().returncode, 0)
        self.assertFalse((self.output / "serving.json").exists())


if __name__ == "__main__":
    unittest.main()
