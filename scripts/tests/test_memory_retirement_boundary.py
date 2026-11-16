import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('retirement',ROOT/'scripts/check_memory_retirement_boundary.py')
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
class RetirementBoundaryTests(unittest.TestCase):
 def test_current_boundary(self):gate.validate(ROOT)
 def fixture(self,root):
  for name in ['server-go/modules/memory','src/tests','tests/baselines/modules']:
   shutil.copytree(ROOT/name,root/name,ignore=shutil.ignore_patterns('build','__pycache__'))
 def test_restored_owner_or_unclassified_fixture_cannot_pass(self):
  for name,text in [('server-go/modules/memory/owner.go','package memory'),('src/tests/unclassified.c','module_runtime_fixture_call(NULL,NULL);')]:
   with tempfile.TemporaryDirectory() as d:
    root=Path(d);self.fixture(root);(root/name).write_text(text)
    with self.assertRaises(ValueError):gate.validate(root)
if __name__=='__main__':unittest.main()
