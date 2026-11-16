import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("module_egress", ROOT / "scripts/check-module-egress.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class ModuleEgressTests(unittest.TestCase):
    def test_retired_owner_cannot_be_restored_even_without_network_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            modules = Path(directory)
            self.assertEqual(checker.retired_client_failures(modules), [])
            (modules / "memory").mkdir()
            (modules / "memory/embed.go").write_text("package memory")
            self.assertEqual(checker.retired_client_failures(modules), [
                "retired concrete memory egress client restored: memory/embed.go"])

    def test_new_client_keeps_direct_network_rejection(self):
        source = 'package client\nimport "net/http"\nfunc x(c *http.Client, r *http.Request) { c.Do(r) }'
        self.assertIsNotNone(checker.violation("new/client.go", source))
        self.assertIsNone(checker.violation("egress/http.go", source))


if __name__ == "__main__":
    unittest.main()
