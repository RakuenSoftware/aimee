"""Keep the current-layout deployment gate sensitive to lost data and authority."""
import importlib.util
from pathlib import Path
import unittest
import tempfile
import os
from unittest.mock import Mock
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
    def test_cli_passes_only_explicit_reference_to_stack(self):
        for selected in (None, '/external/aimee-module-memory'):
            with self.subTest(selected=selected), tempfile.TemporaryDirectory() as directory:
                arguments = ['kb-upgrade-e2e.py', '--output', directory]
                if selected:
                    arguments += ['--reference-memory-owner', selected]
                stack = Mock()
                def exercise(candidate, check):
                    self.assertIs(candidate, stack)
                    check('fixture ready', True)
                with patch('sys.argv', arguments), patch.object(gate.os, 'geteuid', return_value=0), \
                     patch.dict(os.environ, dict(AIMEE_APPLICATION_IMAGE='app',
                                AIMEE_POSTGRES_IMAGE='pg', AIMEE_EMBEDDER_IMAGE='embed')), \
                     patch.object(gate.matrix, 'Stack', return_value=stack) as factory, \
                     patch.object(gate, 'exercise', side_effect=exercise):
                    self.assertEqual(gate.main(), 0)
                self.assertEqual(factory.call_args.args[3], Path(selected) if selected else None)
                stack.compose.assert_called_once_with('down', '--volumes', '--remove-orphans')

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
