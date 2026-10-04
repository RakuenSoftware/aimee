"""The generated environment reference covers both native and Go runtime owners."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('reference_docs', ROOT / 'scripts/gen-reference-docs.py')
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


class EnvironmentReferenceTests(unittest.TestCase):
    def test_native_and_go_reads_exclude_tests_and_dynamic_prefixes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = {
                'src/runtime.c': 'getenv("AIMEE_NATIVE_PROBE");',
                'src/tests/probe.c': 'getenv("AIMEE_NATIVE_TEST_ONLY");',
                'server-go/owner.go': 'os.Getenv("AIMEE_GO_PROBE"); os.LookupEnv("AIMEE_GO_LOOKUP"); os.Getenv("AIMEE_DYNAMIC_" + role)',
                'server-go/owner_test.go': 'os.Getenv("AIMEE_GO_TEST_ONLY")',
                'server-go/testdata/owner.go': 'os.Getenv("AIMEE_FIXTURE_ONLY")',
                'runtime-web/main.go': 'os.Getenv("AIMEE_WEB_PROBE")',
                'control-web/main.go': 'os.Getenv("AIMEE_CONTROL_PROBE")',
            }
            for name, content in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            with patch.object(reference, 'ROOT', root), patch.object(reference, 'SRC', root / 'src'):
                found = reference.parse_env_vars() - reference.ENV_DYNAMIC
            self.assertEqual(found, {'AIMEE_NATIVE_PROBE', 'AIMEE_GO_PROBE', 'AIMEE_GO_LOOKUP',
                                     'AIMEE_WEB_PROBE', 'AIMEE_CONTROL_PROBE'})


if __name__ == '__main__':
    unittest.main()
