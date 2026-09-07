"""Unrelated updates to a PR with model changes must not rebuild its images."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/model-image-inputs-changed.py'
spec = importlib.util.spec_from_file_location('model_inputs', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ModelInputTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        self.git('init', '-q')
        self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.commit({'Dockerfile.llm': 'FROM base@sha256:old\n',
                     'Dockerfile.embedder': 'FROM base@sha256:old\n'})
        self.base = self.git('rev-parse', 'HEAD')

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.repo, text=True).strip()

    def commit(self, files):
        for name, content in files.items():
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        self.git('add', '.')
        self.git('-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'fixture')
        return self.git('rev-parse', 'HEAD')

    def classify(self, kind, before, after='HEAD'):
        result = subprocess.run(['python3', '-I', str(SCRIPT), kind, before, after],
                                cwd=self.repo, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def test_application_update_does_not_repeat_earlier_model_build(self):
        image_change = self.commit({'Dockerfile.llm': 'FROM base@sha256:new\n',
                                    'Dockerfile.embedder': 'FROM base@sha256:new\n'})
        self.commit({'tests/e2e/recall.py': 'new readiness check\n',
                     'src/application.c': 'new application code\n'})
        for kind in module.INPUTS:
            with self.subTest(kind=kind):
                self.assertEqual(self.classify(kind, self.base), 'changed=true')
                self.assertEqual(self.classify(kind, image_change), 'changed=false')

    def test_each_image_input_triggers_only_its_owner(self):
        for kind, paths in module.INPUTS.items():
            for path in paths:
                with self.subTest(kind=kind, path=path):
                    before = self.git('rev-parse', 'HEAD')
                    self.commit({path: 'updated image input\n'})
                    self.assertEqual(self.classify(kind, before), 'changed=true')
                    other = 'embedder' if kind == 'llm' else 'llm'
                    self.assertEqual(self.classify(other, before), 'changed=false')

    def test_workflow_edits_and_unused_model_dockerfile_do_not_rebuild(self):
        self.commit({'.github/workflows/publish-llm.yml': 'updated workflow\n',
                     '.github/workflows/publish-embedder.yml': 'updated workflow\n',
                     'Dockerfile.model': 'unused by sidecar builds\n',
                     'scripts/fetch-synthesis-model.sh': 'unused by sidecar builds\n'})
        for kind in module.INPUTS:
            self.assertEqual(self.classify(kind, self.base), 'changed=false')

    def test_missing_revision_fails_without_authorizing_a_build(self):
        result = subprocess.run(['python3', '-I', str(SCRIPT), 'llm', 'missing', 'HEAD'],
                                cwd=self.repo, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('changed=true', result.stdout)

    def test_reverted_inputs_do_not_rebuild(self):
        self.commit({'Dockerfile.llm': 'temporary change\n'})
        self.commit({'Dockerfile.llm': 'FROM base@sha256:old\n'})
        self.assertEqual(self.classify('llm', self.base), 'changed=false')

    def test_workflows_gate_builds_before_registry_or_builder_setup(self):
        for kind, paths in module.INPUTS.items():
            with self.subTest(kind=kind):
                workflow = yaml.load((ROOT / f'.github/workflows/publish-{kind}.yml').read_text(),
                                     Loader=yaml.BaseLoader)
                self.assertEqual(set(workflow['on']['pull_request']['paths']), set(paths))
                self.assertNotIn('force', workflow['on']['workflow_dispatch'])
                self.assertNotIn('concurrency', workflow)
                changes = workflow['jobs']['changes']
                step = changes['steps'][1]
                self.assertIn('github.event.before', step['env']['BASE_SHA'])
                self.assertIn(f'model-image-inputs-changed.py {kind}', step['run'])
                build = workflow['jobs']['build']
                self.assertEqual(build['needs'], 'changes')
                self.assertIn("needs.changes.outputs.changed == 'true'", build['if'])
                self.assertIn("inputs.version != ''", build['if'])
                self.assertIn("github.event_name == 'workflow_dispatch'", build['if'])
                self.assertIn('matrix.name', build['concurrency']['group'])


if __name__ == '__main__':
    unittest.main()
