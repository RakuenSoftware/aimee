"""Keep the two tools owners on the same reviewed metadata contract."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('tool_catalog', ROOT / 'scripts/generate-tool-catalog.py')
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class ToolCatalogTests(unittest.TestCase):
    def test_generated_c_owner_matches_shared_catalog(self):
        self.assertEqual(generator.TARGET.read_text(), generator.generate())

    def test_duplicate_fields_are_not_silently_replaced(self):
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            generator.unique([('bash', 'read'), ('bash', 'write')])


if __name__ == '__main__':
    unittest.main()
