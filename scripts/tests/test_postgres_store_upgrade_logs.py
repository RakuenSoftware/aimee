#!/usr/bin/env python3
"""Exercise the upgrade assertion with streamed Docker logs under pipefail."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class UpgradeLogAssertionTest(unittest.TestCase):
    def test_stream_is_drained_and_failures_are_preserved(self):
        source = (ROOT / "scripts/tests/test_postgres_store_upgrade.sh").read_text()
        assertion = next(line for line in source.splitlines()
                         if line.startswith('docker logs "$repaired_container"'))
        with tempfile.TemporaryDirectory() as directory:
            producer = pathlib.Path(directory) / "logs.py"
            producer.write_text(
                "import signal, sys\n"
                "signal.signal(signal.SIGPIPE, signal.SIG_DFL)\n"
                "if sys.argv[1] != 'missing':\n"
                "    print('both aimee_shared and aimee_store exist', flush=True)\n"
                "for _ in range(256):\n"
                "    print('subsequent diagnostic ' * 4096, flush=True)\n"
                "sys.exit(7 if sys.argv[1] == 'failed' else 0)\n"
            )
            # Use positional parameters rather than interpolate paths into shell code.
            command = ('set -euo pipefail\n'
                       'repaired_container=test\n'
                       'producer=$1; mode=$2\n'
                       'docker() { python3 "$producer" "$mode"; }\n' + assertion)
            for mode, expected in [('matched', 0), ('missing', 1), ('failed', 7)]:
                with self.subTest(mode=mode):
                    result = subprocess.run(['bash', '-c', command, 'test', str(producer), mode],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
