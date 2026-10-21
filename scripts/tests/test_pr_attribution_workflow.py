"""Metadata edits must retain prompt attribution checks and source qualification."""
from pathlib import Path
import json
import shutil
import subprocess
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


def workflow(name):
    return yaml.load((ROOT / '.github/workflows' / name).read_text(), Loader=yaml.BaseLoader)


class AttributionWorkflowTests(unittest.TestCase):
    def test_permanent_check_is_owned_once_with_live_metadata_and_read_only_permissions(self):
        owners = []
        for path in (ROOT / '.github/workflows').glob('*.yml'):
            data = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
            if 'no-coauthor-trailers' in data.get('jobs', {}):
                owners.append(path.name)
        self.assertEqual(owners, ['pr-attribution.yml'])
        data = workflow(owners[0])
        job = data['jobs']['no-coauthor-trailers']
        self.assertEqual(job.get('name', 'no-coauthor-trailers'), 'no-coauthor-trailers')
        self.assertNotIn('if', job)
        self.assertEqual(data['permissions'], {'contents': 'read'})
        self.assertEqual(job['permissions'], {'contents': 'read', 'issues': 'read', 'pull-requests': 'read', 'checks': 'read'})
        steps = job['steps']
        live = next(step for step in steps if step.get('run', '').startswith('go run '))
        self.assertTrue(any(step.get('run') ==
                            'go test ../scripts/check-pr-attribution.go ../scripts/check-pr-attribution_test.go'
                            for step in steps))
        self.assertEqual(live['run'], 'go run ../scripts/check-pr-attribution.go')
        self.assertEqual(live['env']['GH_TOKEN'], '${{ github.token }}')
        self.assertNotIn('ATTRIBUTION_RE', (ROOT / '.github/workflows/pr-attribution.yml').read_text())
        self.assertEqual(steps[0]['with']['persist-credentials'], 'false')
        self.assertNotIn('continue-on-error', job)
        for step in steps:
            self.assertNotIn('continue-on-error', step)

    def test_metadata_and_queue_events_preserve_the_existing_check_surface(self):
        source, attribution = workflow('ci.yml'), workflow('pr-attribution.yml')
        for event in ('push', 'merge_group'):
            self.assertEqual(source['on'][event], attribution['on'][event])
        self.assertNotIn('branches', attribution['on']['pull_request'])
        for field in ('base_sha', 'head_sha'):
            self.assertEqual(attribution['on']['workflow_dispatch']['inputs'][field]['required'], 'true')
        self.assertIn('edited', attribution['on']['pull_request']['types'])
        self.assertEqual(attribution['concurrency']['cancel-in-progress'], 'true')
        self.assertNotEqual(source['concurrency']['group'], attribution['concurrency']['group'])
        # Do not turn edited events into successful skipped required source jobs.
        for job in source['jobs'].values():
            self.assertNotIn('github.event.action', job.get('if', ''))

    def test_source_cancellation_keeps_metadata_edits_but_restarts_changed_bases(self):
        # The actual workflow expression deliberately uses only boolean operators
        # and null comparisons shared with JavaScript. Execute that expression,
        # rather than duplicating its decision as a Python helper. Absent change
        # properties are undefined, which compares equal to null in both runtimes.
        expression = workflow('ci.yml')['concurrency']['cancel-in-progress'].strip()
        self.assertTrue(expression.startswith('${{') and expression.endswith('}}'))
        expression = expression[3:-2]
        node = shutil.which('node')
        self.assertIsNotNone(node, 'Node is required to exercise the workflow expression')
        cases = [
            ('push', '', {}, True),
            ('merge_group', 'checks_requested', {}, True),
            ('pull_request', 'synchronize', {}, True),
            ('pull_request', 'opened', {}, True),
            ('pull_request', 'edited', {'title': {'from': 'old'}}, False),
            ('pull_request', 'edited', {'body': {'from': 'old'}}, False),
            ('pull_request', 'edited', {'title': {}, 'body': {}}, False),
            ('pull_request', 'edited', {'base': {'ref': {'from': 'testing'}}}, True),
            ('pull_request', 'edited', {'base': {}, 'body': {}}, True),
            ('pull_request', 'edited', {}, True),
        ]
        for event, action, changes, expected in cases:
            with self.subTest(event=event, action=action, changes=changes):
                context = {'event_name': event, 'event': {'action': action, 'changes': changes}}
                script = 'const github = JSON.parse(process.argv[1]); console.log(JSON.stringify(' + expression + '));'
                result = subprocess.run([node, '-e', script, json.dumps(context)],
                                        text=True, capture_output=True, timeout=10, check=True)
                self.assertEqual(json.loads(result.stdout), expected)


if __name__ == '__main__':
    unittest.main()
