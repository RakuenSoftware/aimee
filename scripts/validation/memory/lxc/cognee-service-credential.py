#!/usr/bin/env python3
"""Replace expiring lab session credentials with a pinned Cognee service API key."""
import json,subprocess,urllib.parse,urllib.request
from pathlib import Path
root=Path('/var/lib/aimee-provider')
password=(root/'account-password').read_text().strip()
request=urllib.request.Request('http://127.0.0.1:8097/api/v1/auth/login',data=urllib.parse.urlencode({'username':'default_user@example.com','password':password}).encode(),headers={'Content-Type':'application/x-www-form-urlencoded'})
with urllib.request.urlopen(request,timeout=15) as response:session=json.load(response)['access_token']
request=urllib.request.Request('http://127.0.0.1:8097/api/v1/auth/api-keys',data=json.dumps({'name':'aimee-memory-lxc-service'}).encode(),headers={'Authorization':'Bearer '+session,'Content-Type':'application/json'})
with urllib.request.urlopen(request,timeout=15) as response:credential=json.load(response)
key=credential['key'];(root/'token').write_text(key);(root/'token').chmod(0o600)
(root/'service-key-id').write_text(credential['id']);(root/'service-key-id').chmod(0o600)
state_path=Path('/var/lib/aimee-memory-lab/resume-private.json');state=json.loads(state_path.read_text())
subprocess.run(['systemctl','stop','aimee-memory-validation'],check=True)
try:
 for role in ['server','kb']:
  env=state[role]['env'].copy();env['AIMEE_MEMORY_BACKEND_AUTH']='api-key';env['AIMEE_VAULT_ENV_OVERWRITE']='1';env.pop('AIMEE_MEMORY_BACKEND_TOKEN',None)
  result=subprocess.run(['/opt/aimee/aimee-'+role,'--bootstrap-vault-stdin'],env=env,input='AIMEE_MEMORY_BACKEND_TOKEN='+key+'\0',text=True,capture_output=True)
  if result.returncode:raise RuntimeError('fixture Vault API-key import failed')
 for component in state.values():component['env']['AIMEE_MEMORY_BACKEND_AUTH']='api-key';component['env'].pop('AIMEE_MEMORY_BACKEND_TOKEN',None)
 state_path.write_text(json.dumps(state));state_path.chmod(0o600)
finally:subprocess.run(['systemctl','start','aimee-memory-validation'],check=True)
print('Cognee durable service key imported through both existing fixture Vaults; no credentials published.')
