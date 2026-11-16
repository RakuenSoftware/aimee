"""Exercise the required Windows context against actual dependency outcomes."""
import os
from pathlib import Path
import subprocess
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class WindowsCMakeGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workflow = yaml.load((ROOT / '.github/workflows/ci.yml').read_text(),
                             Loader=yaml.BaseLoader)
        cls.gate = workflow['jobs']['windows-cmake']
        cls.script = cls.gate['steps'][0]['run']

    def run_gate(self, windows, scope='success', docs_only='false'):
        return subprocess.run(['bash', '-c', self.script], capture_output=True,
                              env=dict(os.environ, WINDOWS_RESULT=windows,
                                       SCOPE_RESULT=scope, DOCS_ONLY=docs_only))

    def test_required_context_depends_on_the_real_windows_job(self):
        self.assertEqual(self.gate['name'], 'windows-cmake')
        self.assertCountEqual(self.gate['needs'], ['change-scope', 'windows-build'])
        self.assertEqual(self.gate['if'], '${{ always() }}')
        env = self.gate['steps'][0]['env']
        self.assertEqual(env['WINDOWS_RESULT'], '${{ needs.windows-build.result }}')
        self.assertEqual(env['SCOPE_RESULT'], '${{ needs.change-scope.result }}')
        self.assertEqual(env['DOCS_ONLY'], '${{ needs.change-scope.outputs.docs_only }}')

    def test_successful_windows_build_qualifies(self):
        self.assertEqual(self.run_gate('success').returncode, 0)

    def test_failed_cancelled_or_unknown_build_never_qualifies(self):
        for result in ('failure', 'cancelled', '', 'unknown'):
            with self.subTest(result=result):
                self.assertNotEqual(self.run_gate(result).returncode, 0)

    def test_skip_requires_a_successful_documentation_only_classification(self):
        self.assertEqual(self.run_gate('skipped', docs_only='true').returncode, 0)
        for scope, docs in (('success', 'false'), ('failure', 'true'),
                            ('cancelled', 'true'), ('', 'true'), ('success', '')):
            with self.subTest(scope=scope, docs=docs):
                self.assertNotEqual(self.run_gate('skipped', scope, docs).returncode, 0)


if __name__ == '__main__':
    unittest.main()
