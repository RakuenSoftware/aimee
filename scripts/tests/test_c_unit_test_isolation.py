"""A test resetting HOME must not remove a parallel test's Git metadata."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

WRAPPER = Path(__file__).resolve().parents[1] / 'run-c-unit-test.sh'

class IsolationTest(unittest.TestCase):
    def test_parallel_home_reset_cannot_remove_git_worktree_metadata(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            worker = root / 'worker.py'
            worker.write_text('''import os, pathlib, shutil, sys, time
root=pathlib.Path(sys.argv[1]); home=pathlib.Path(os.environ['HOME'])
if sys.argv[2]=='git':
    metadata=home/'.git/worktrees/child';metadata.mkdir(parents=True)
    (root/'ready').touch()
    deadline=time.monotonic()+5
    while not (root/'removed').exists():
        assert time.monotonic()<deadline
        time.sleep(.01)
    (metadata/'locked').write_text('worktree initialization')
else:
    deadline=time.monotonic()+5
    while not (root/'ready').exists():
        assert time.monotonic()<deadline
        time.sleep(.01)
    shutil.rmtree(home)
    (root/'removed').touch()
''')
            env = dict(os.environ, HOME=str(root / 'shared'), TMPDIR=str(root / 'shared'))
            (root / 'shared').mkdir()
            processes = [subprocess.Popen([str(WRAPPER), sys.executable, str(worker), str(root), mode],
                                          env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                         for mode in ('git', 'reset')]
            for p in processes:
                stdout, stderr = p.communicate(timeout=10)
                self.assertEqual(p.returncode, 0, (stdout, stderr))
            self.assertTrue((root / 'shared').is_dir())

    def test_exit_status_is_preserved(self):
        p = subprocess.run([str(WRAPPER), sys.executable, '-c', 'raise SystemExit(23)'])
        self.assertEqual(p.returncode, 23)

if __name__ == '__main__':
    unittest.main()
