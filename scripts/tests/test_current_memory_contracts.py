import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.check_current_memory_contracts import current_cases, MANIFEST


class CurrentServingBoundaryTests(unittest.TestCase):
    def test_all_remaining_identities_are_preserved_and_exactly_ten_are_extracted(self):
        manifest = json.loads(MANIFEST.read_text())
        current, extracted = current_cases(manifest)
        self.assertEqual(len(current), 163)
        self.assertEqual(len(extracted), 10)
        self.assertEqual({case['id'] for case in current + extracted},
                         {case['id'] for group in manifest['groups'] for case in group['cases']})
        bad = copy.deepcopy(manifest)
        bad['groups'][0]['cases'][0]['package'] = './some-new-owner'
        with self.assertRaises(ValueError):
            current_cases(bad)

    def test_old_full_certification_is_explicitly_unavailable_and_writes_no_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'old-pass.json'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/check_memory_reliability_release.py'), '--output', str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('full historical MR18 certification is unavailable', result.stderr)
            self.assertFalse(output.exists())

    def test_retired_current_serving_gate_cannot_write_a_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'pass.json'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/check_current_memory_contracts.py'), '--output', str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('serving-owner certification is unavailable', result.stderr)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
