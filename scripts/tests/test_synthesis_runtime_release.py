"""Exercise the actual release gate with simulated forge and registry results."""
import os
import importlib.util
import json
import shutil
from unittest.mock import patch
from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class RuntimeReleaseTests(unittest.TestCase):
    def run_gate(self, published='b10219', registry='missing', forge_exit='0'):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commands = {
                'gh': '#!/bin/sh\nprintf "%s\\n" "$PUBLISHED"\nexit "$FORGE_EXIT"\n',
                'docker': '''#!/bin/sh
case "$REGISTRY" in
 existing) echo '{}' ;;
 missing) echo 'no such manifest: fixture' >&2; exit 1 ;;
 outage) echo 'TLS handshake timeout' >&2; exit 1 ;;
 denied) echo 'unauthorized' >&2; exit 1 ;;
esac
'''}
            for name, content in commands.items():
                path = root / name
                path.write_text(content)
                path.chmod(0o755)
            return subprocess.run(['bash', str(ROOT / 'scripts/check-synthesis-runtime-release.sh'),
                                   'ghcr.io/fixture/synthesis', 'b10219'],
                                  env=dict(os.environ, PATH=directory+':'+os.environ['PATH'],
                                           PUBLISHED=published, REGISTRY=registry, FORGE_EXIT=forge_exit),
                                  capture_output=True, text=True)

    def test_workflow_requires_release_gate_and_cpu_probe(self):
        workflow = yaml.load((ROOT / '.github/workflows/publish-llm.yml').read_text(), Loader=yaml.BaseLoader)
        steps = workflow['jobs']['build']['steps']
        gate = next(i for i, step in enumerate(steps)
                    if 'check-synthesis-runtime-release.sh' in step.get('run', ''))
        build = next(i for i, step in enumerate(steps)
                     if step.get('with', {}).get('file') == 'Dockerfile.llm')
        probe = next(i for i, step in enumerate(steps)
                     if 'test-synthesis-portability.sh' in step.get('run', ''))
        self.assertLess(gate, build)
        self.assertGreater(probe, build)
        self.assertIn(':runtime-${{ steps.have.outputs.llamacpp_version }}', steps[build]['with']['tags'])
        self.assertIn("steps.have.outputs.exists != 'true'", steps[gate]['if'])
        self.assertIn("inputs.version == ''", steps[gate]['if'])
        source = (ROOT / 'Dockerfile.llm').read_text()
        for flag in ('GGML_NATIVE=OFF', 'GGML_BACKEND_DL=ON', 'GGML_CPU_ALL_VARIANTS=ON'):
            self.assertIn('-D' + flag, source)

    def test_new_published_runtime_and_missing_image_authorize_build(self):
        self.assertEqual(self.run_gate().returncode, 0)

    def test_same_runtime_never_rebuilds(self):
        self.assertNotEqual(self.run_gate(registry='existing').returncode, 0)

    def test_unpublished_runtime_and_forge_failure_refuse(self):
        for kwargs in ({'published': ''}, {'published': 'b10218'}, {'forge_exit': '1'}):
            self.assertNotEqual(self.run_gate(**kwargs).returncode, 0)

    def test_registry_errors_cannot_authorize_a_build(self):
        for registry in ('outage', 'denied'):
            self.assertNotEqual(self.run_gate(registry=registry).returncode, 0)


class PromotionSourceTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('promotion', ROOT / 'scripts/synthesis-promotion-source.py')
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for name in (*self.module.INPUTS, 'dependencies/synthesis-images.lock.json'):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        original = subprocess.check_output
        def command(args, **kwargs):
            if args[0] == 'git':
                kwargs['cwd'] = ROOT
            return original(args, **kwargs)
        self.mock = patch.object(self.module.subprocess, 'check_output', side_effect=command)
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def test_resolves_both_validated_images_despite_queued_wrapper_edits(self):
        for image in ('aimee-llm-e2b', 'aimee-llm-e4b'):
            tag, digest = self.module.resolve(image, self.root)
            self.assertIn('-b10219-7bc9d6c31053', tag)
            self.assertIn('@sha256:', digest)

    def test_runtime_or_weight_changes_require_requalification(self):
        for path in ('Dockerfile.llm', 'scripts/synthesis-model-table.sh'):
            target = self.root / path
            saved = target.read_text()
            target.write_text(saved.replace('b10219', 'b10220') if path.endswith('llm') else saved + '# changed\n')
            with self.assertRaises(ValueError):
                self.module.resolve('aimee-llm-e2b', self.root)
            target.write_text(saved)

    def test_bad_source_tag_and_digest_refuse(self):
        path = self.root / 'dependencies/synthesis-images.lock.json'
        original = json.loads(path.read_text())
        for field, value in (('source_commit', 'main'), ('tag', 'testing'), ('digest', 'latest')):
            lock = json.loads(json.dumps(original))
            if field == 'source_commit':
                lock[field] = value
            else:
                lock['images']['aimee-llm-e2b'][field] = value
            path.write_text(json.dumps(lock))
            with self.assertRaises(ValueError):
                self.module.resolve('aimee-llm-e2b', self.root)

    def test_workflow_checks_registry_digest_before_promotion(self):
        workflow = yaml.load((ROOT / '.github/workflows/publish-llm.yml').read_text(), Loader=yaml.BaseLoader)
        steps = workflow['jobs']['build']['steps']
        verify = next(i for i, step in enumerate(steps) if step.get('name') == 'Verify locked source tag and digest')
        promote = next(i for i, step in enumerate(steps) if step.get('name', '').startswith('Promote to'))
        self.assertLess(verify, promote)
        script = steps[verify]['run'].replace('${{ steps.have.outputs.source_tag }}', 'ghcr.io/fixture:immutable').replace('${{ steps.have.outputs.pinned }}', 'ghcr.io/fixture@sha256:expected')
        docker = self.root / 'docker'
        docker.write_text('#!/bin/sh\nprintf "%s\\n" "$ACTUAL"\n')
        docker.chmod(0o755)
        for digest, expected in (('sha256:expected', 0), ('sha256:changed', 1)):
            result = subprocess.run(['bash', '-c', script], env=dict(os.environ, PATH=str(self.root)+':'+os.environ['PATH'], ACTUAL=digest), capture_output=True)
            self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == '__main__':
    unittest.main()
