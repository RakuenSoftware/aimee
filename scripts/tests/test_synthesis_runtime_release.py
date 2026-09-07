"""Exercise the actual release gate with simulated forge and registry results."""
import os
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


if __name__ == '__main__':
    unittest.main()
