import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.check_memory_reliability_release import verify_events


class ReleaseExecutionTests(unittest.TestCase):
    def test_actual_pass_required(self):
        self.assertEqual(verify_events([{'Action': 'pass', 'Test': 'TestOne'}], {'TestOne'}), {'TestOne': 'pass'})
        for events in ([], [{'Action': 'skip', 'Test': 'TestOne'}],
                       [{'Action': 'pass', 'Test': 'TestOne'}, {'Action': 'skip', 'Test': 'TestOne/required_database'}],
                       [{'Action': 'pass', 'Test': 'TestOne'}, {'Action': 'fail'}]):
            with self.assertRaises(ValueError):
                verify_events(events, {'TestOne'})


if __name__ == "__main__":
    unittest.main()
