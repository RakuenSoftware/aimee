"""Regression for routed retirement being invisible in the served manifest."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cli_help_coverage", ROOT / "scripts/check-cli-help-coverage.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class MemoryDeleteHelpTest(unittest.TestCase):
    def test_actual_help_exposes_routed_delete(self):
        routes = check.ROUTES.read_text()
        help_text = check.HELP.read_text()
        self.assertFalse(check.missing_memory_delete_help(routes, help_text))
        without_delete = '\n'.join(line for line in help_text.splitlines()
                                   if '"  delete <id>' not in line)
        self.assertTrue(check.missing_memory_delete_help(routes, without_delete))

    def test_other_family_cannot_satisfy_memory_help(self):
        routes = '{"memory", "delete", "memory.delete", NULL, NULL, 60000},'
        help_text = ('{"memory", "Stored memory", CORE, 0, "  get <id>\\n"},\n'
                     '{"rules", "Rules", CORE, 0, "  delete <id>\\n"},')
        self.assertTrue(check.missing_memory_delete_help(routes, help_text))


if __name__ == "__main__":
    unittest.main()
