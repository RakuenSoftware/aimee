from pathlib import Path
from tempfile import TemporaryDirectory
import importlib.util
import unittest

# Loaded BY PATH, the way every other test in this directory loads its subject.
#
# This used to be `from scripts.check_go_module_boundary import violations`,
# which needs the repository root on sys.path and therefore cannot run under
# `python3 -I` -- isolated mode drops the working directory. Every workflow here
# invokes these tests with -I, so this one could not be wired in the ordinary
# way, and it was not: it ran nowhere at all. The import style was the reason
# the test was unreachable, not an incidental difference.
_SOURCE = Path(__file__).resolve().parent.parent / "check_go_module_boundary.py"
_SPEC = importlib.util.spec_from_file_location("check_go_module_boundary", _SOURCE)
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)
violations = _MODULE.violations


class GoModuleBoundaryTest(unittest.TestCase):
    def test_peer_import_is_rejected_but_own_subpackage_is_not(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "alpha" / "detail").mkdir(parents=True)
            (root / "beta").mkdir()
            (root / "alpha" / "ok.go").write_text(
                'package alpha\nimport "github.com/JBailes/aimee/server-go/modules/alpha/detail"\n')
            (root / "alpha" / "bad.go").write_text(
                'package alpha\nimport "github.com/JBailes/aimee/server-go/modules/beta"\n')
            self.assertEqual(len(violations(root)), 1)


if __name__ == "__main__":
    unittest.main()
