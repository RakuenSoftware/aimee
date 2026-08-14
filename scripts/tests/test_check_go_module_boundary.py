from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from scripts.check_go_module_boundary import violations


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
