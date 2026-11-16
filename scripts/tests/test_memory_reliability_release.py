import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import json
import tempfile

from scripts.check_memory_reliability_release import import_path, load_recorded_events, verify_events


class ReleaseExecutionTests(unittest.TestCase):
    def test_actual_pass_required(self):
        self.assertEqual(verify_events([{'Action': 'pass', 'Test': 'TestOne'}], {'TestOne'}), {'TestOne': 'pass'})
        for events in ([], [{'Action': 'skip', 'Test': 'TestOne'}],
                       [{'Action': 'pass', 'Test': 'TestOne'}, {'Action': 'skip', 'Test': 'TestOne/required_database'}],
                       [{'Action': 'pass', 'Test': 'TestOne'}, {'Action': 'fail'}]):
            with self.assertRaises(ValueError):
                verify_events(events, {'TestOne'})

    def test_recorded_events_require_a_finished_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'events.json'
            rows = [
                {'Action': 'pass', 'Package': 'example/done', 'Test': 'TestOne'},
                {'Action': 'pass', 'Package': 'example/done'},
                {'Action': 'pass', 'Package': 'example/cut', 'Test': 'TestTwo'},
            ]
            path.write_text('\n'.join(json.dumps(row) for row in rows) + '\nbuild noise\n')
            recorded = load_recorded_events([path])
        self.assertEqual(sorted(recorded), ['example/done'])
        self.assertEqual(verify_events(recorded['example/done'], {'TestOne'}), {'TestOne': 'pass'})
        with self.assertRaises(ValueError):
            verify_events(recorded['example/done'], {'TestOne', 'TestMissing'})

    def test_manifest_package_maps_to_go_test_import_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'go.mod').write_text('module example.com/repo/server-go\n\ngo 1.26\n')
            self.assertEqual(import_path('./modules/memory', Path(tmp)),
                             'example.com/repo/server-go/modules/memory')
            self.assertEqual(import_path('./cmd/aimee-module', Path(tmp)),
                             'example.com/repo/server-go/cmd/aimee-module')


if __name__ == "__main__":
    unittest.main()
