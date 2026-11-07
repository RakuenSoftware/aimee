"""Queue events have no pull_request payload and must run the complete gate."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class MergeQueueTests(unittest.TestCase):
    def test_queue_event_without_pr_metadata_runs_full_suite(self):
        workflow = yaml.load((ROOT / '.github/workflows/ci.yml').read_text(),
                             Loader=yaml.BaseLoader)
        step = next(step for step in workflow['jobs']['change-scope']['steps']
                    if step.get('id') == 'classify')
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'output'
            env = dict(os.environ, EVENT_NAME='merge_group', BASE_SHA='', HEAD_SHA='',
                       GITHUB_OUTPUT=str(output))
            # No git repository: accidentally falling through to the PR diff path fails.
            result = subprocess.run(['bash', '-euo', 'pipefail', '-c', step['run']],
                                    cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_text().splitlines(),
                             ['docs_only=false', 'docs_changed=true'])

    def test_queue_workflows_preserve_integration_checkout(self):
        for name in ('ci', 'release-policy', 'core-c', 'c-repositories', 'audit-storage'):
            with self.subTest(workflow=name):
                workflow = yaml.load((ROOT / f'.github/workflows/{name}.yml').read_text(),
                                     Loader=yaml.BaseLoader)
                trigger = workflow['on']['merge_group']
                self.assertEqual(trigger['types'], ['checks_requested'])
                self.assertEqual(trigger['branches'], ['testing'])
                self.assertNotIn('paths', trigger)
                for job in workflow['jobs'].values():
                    for step in job.get('steps', []):
                        if step.get('uses', '').startswith('actions/checkout@'):
                            # No head ref override: checkout must retain the event's merge SHA.
                            self.assertNotIn('ref', step.get('with', {}))

    def test_queue_does_not_publish_or_deploy(self):
        for path in (ROOT / '.github/workflows').glob('*.yml'):
            workflow = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
            if 'merge_group' not in workflow.get('on', {}):
                continue
            self.assertEqual(workflow['permissions'], {'contents': 'read'}, path.name)
            for job in workflow['jobs'].values():
                for permission in job.get('permissions', {}).values():
                    self.assertNotEqual(permission, 'write', path.name)


if __name__ == '__main__':
    unittest.main()
