"""Keep the current-layout deployment gate sensitive to lost data and authority."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('kb_recreation', ROOT / 'tests/e2e/kb-upgrade-e2e.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class Stack:
    def __init__(self, defect=None):
        self.records = {}
        self.starts = 0
        self.downs = 0
        self.defect = defect

    def start(self):
        self.starts += 1

    def compose(self, action):
        assert action == 'down'
        self.downs += 1
        if self.defect == 'lost-content':
            self.records.clear()
        if self.defect == 'authority':
            for record in self.records.values():
                record['provenance_category'] = 'user_stated'

    def kb_request(self, path, body):
        if path.endswith('memory.store'):
            mid = len(self.records) + 1
            self.records[str(mid)] = dict(id=mid, content=body['content'],
                provenance_category='agent_message', confidence=.8)
            return 200, dict(status='ok', id=mid)
        return 200, dict(status='ok', memory=dict(self.records.get(body['id'], {})))


class KBRecreationTests(unittest.TestCase):
    def run_gate(self, stack):
        def check(name, passed):
            if not passed:
                raise AssertionError(name)
        with patch.object(gate.matrix, 'application_metadata_is_private', return_value=True):
            gate.exercise(stack, check)

    def test_current_store_survives_two_recreations(self):
        stack = Stack()
        self.run_gate(stack)
        self.assertEqual((stack.starts, stack.downs, len(stack.records)), (3, 2, 4))

    def test_data_or_authority_loss_fails_the_gate(self):
        for defect in ('lost-content', 'authority'):
            with self.subTest(defect=defect):
                with self.assertRaisesRegex(AssertionError, 'retains global content and authority'):
                    self.run_gate(Stack(defect))


if __name__ == '__main__':
    unittest.main()
