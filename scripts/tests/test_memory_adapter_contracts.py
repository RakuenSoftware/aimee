import importlib.util
from pathlib import Path
import unittest
p=Path(__file__).resolve().parents[1]/'check_memory_adapter_contracts.py'
spec=importlib.util.spec_from_file_location('adapter_gate',p)
gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
class AdapterGateTests(unittest.TestCase):
 def test_missing_skipped_or_failed_cases_refuse(self):
  for events in [[],[{'Action':'skip','Test':'required'}],[{'Action':'fail','Test':'required'}]]:
   with self.assertRaises(ValueError):gate.verify(events,['required'])
 def test_exact_success_includes_subtest_evidence(self):
  self.assertEqual(gate.verify([{'Action':'pass','Test':'required'},{'Action':'pass','Test':'required/canonical'}],['required']),['required','required/canonical'])
if __name__=='__main__':unittest.main()
