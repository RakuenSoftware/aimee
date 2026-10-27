import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(SCRIPTS))
import validate_module_process_contracts as contract
import export_c_repositories as exporter

class NativeHostContracts(unittest.TestCase):
 def mutated(self,mutate):
  document=copy.deepcopy(contract.load(contract.CONTRACTS));mutate(document)
  original=contract.load
  with patch.object(contract,"load",side_effect=lambda p:document if p==contract.CONTRACTS else original(p)):
   return contract.validate()
 def test_only_exact_native_stage_can_be_core_hosted(self):
  def wrong(doc):next(x for x in doc['components'] if x['id']=='delegates')['stages'][0]['hosted_by']='server-core'
  with self.assertRaises(contract.ContractError):self.mutated(wrong)
  def remove(doc):next(x for x in doc['components'] if x['id']=='delegates')['stages'][22].pop('hosted_by')
  with self.assertRaises(contract.ContractError):self.mutated(remove)
 def test_native_request_principal_cannot_gain_other_rights(self):
  for field,value in [('serve',[6679]),('request',[6679,4609]),('publish',[3000]),('executable','/usr/local/bin/aimee-wfe')]:
   def mutate(doc):next(x for x in doc['clients'] if x['id']=='delegates-host')[field]=value
   with self.subTest(field=field),self.assertRaises(contract.ContractError):self.mutated(mutate)
 def test_generated_bundle_separates_core_server_and_go_serving_rights(self):
  with tempfile.TemporaryDirectory() as directory:
   bundle=Path(directory)/'bundle';exporter.export_runtime_bundle(bundle)
   go_grant=(bundle/'grants/server/delegates.grant').read_text()
   line=next(x for x in go_grant.splitlines() if x.startswith('serve='))
   self.assertNotIn('6679',line.split('=')[1].split(','));self.assertNotIn('6680',line.split('=')[1].split(','))
   grant=(bundle/'grants/server/delegates-host.grant').read_text()
   self.assertIn('principal_ref=76\n',grant);self.assertIn('request=6679\nserve=\n',grant)
   self.assertNotIn('6680',(bundle/'grants/server/wfe.grant').read_text())
   services=json.loads((bundle/'host-services.json').read_text())['services']
   self.assertEqual(services,[{'owner':'delegates','stage':23,'event_kind':6679,'hosted_by':'server-core','principal_class':0,'principal_ref':0,'placements':['server']}])

if __name__=='__main__':unittest.main()
