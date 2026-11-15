#!/usr/bin/env python3
"""Exercise the deployed health probe with independent core/browser failures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ApplicationHealth(unittest.TestCase):
    def test_enabled_dashboard_must_complete_tls_and_http(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path/'aimee-runtime-web').write_text('#!/bin/sh\nexit 0\n')
            (path/'aimee-runtime-web').chmod(0o755)
            (path/'curl').write_text('''#!/bin/sh
case "$*" in
 *https://*) exit "${FIXTURE_BROWSER_FAILURE:-0}" ;;
 *--unix-socket*) printf '%s' "${FIXTURE_READY:-{\\"ready\\":true}}" ;;
 *) exit 0 ;;
esac
''')
            (path/'curl').chmod(0o755)
            env = dict(os.environ, PATH=str(path)+':/usr/bin:/bin', FIXTURE_READY='{"ready":true}')
            def probe(**changes):
                return subprocess.run(['sh',str(ROOT/'deploy/container/application-health.sh')],env=dict(env,**changes),capture_output=True).returncode
            self.assertEqual(probe(),0)
            self.assertNotEqual(probe(FIXTURE_BROWSER_FAILURE='52'),0)
            self.assertNotEqual(probe(FIXTURE_READY='{"ready":false}'),0)
            self.assertEqual(probe(FIXTURE_BROWSER_FAILURE='52',AIMEE_RUNTIME_WEB_ENABLED='0'),0)
            self.assertEqual(probe(FIXTURE_BROWSER_FAILURE='52',AIMEE_INSTANCE_ROLE='kb'),0)
            (path/'aimee-runtime-web').unlink()
            self.assertEqual(probe(FIXTURE_BROWSER_FAILURE='52'),0)


if __name__ == '__main__':
    unittest.main()
