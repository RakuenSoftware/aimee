#!/usr/bin/env python3
"""Exercise the actual CI export step without replacing published release pins."""
from pathlib import Path
import os
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ExportPinTests(unittest.TestCase):
    def test_fixture_export_restores_published_lock_on_success_and_failure(self):
        workflow = (ROOT / '.github/workflows/c-repositories.yml').read_text()
        step = workflow.split('      - name: Export standalone repositories\n', 1)[1]
        body = step.split('        run: |\n', 1)[1].split('      - name:', 1)[0]
        script = textwrap.dedent(body)
        for exit_code in (0, 17):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / 'dependencies').mkdir()
                (root / 'scripts').mkdir()
                lock = root / 'dependencies/aimee-repositories.lock.json'
                published = b'{"commit":"published-repository-commit"}\n'
                lock.write_bytes(published)
                # The real exporter creates local fixture commits and rewrites
                # the lock before it can finish or fail at a later export.
                (root / 'scripts/export_c_repositories.py').write_text(
                    'from pathlib import Path\n'
                    'Path("dependencies/aimee-repositories.lock.json").write_text("local-fixture")\n'
                    f'raise SystemExit({exit_code})\n')
                result = subprocess.run(['bash', '-c', script], cwd=root,
                                        env={**os.environ, 'RUNNER_TEMP': temp},
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, exit_code, result.stderr)
                self.assertEqual(lock.read_bytes(), published)


if __name__ == '__main__':
    unittest.main()
