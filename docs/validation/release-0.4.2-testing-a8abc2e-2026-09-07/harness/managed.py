import importlib.util,json,os,re,subprocess,time
from pathlib import Path
root=Path('/opt/validation042')
spec=importlib.util.spec_from_file_location('matrix',root/'source/tests/e2e/deployment-matrix.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
env=dict(os.environ,AIMEE_RUNTIME_WEB_ENABLED='1',AIMEE_IMAGE_TAG='testing',AIMEE_POSTGRES_VOLUME_MIB='1024',COMPOSE_PROFILES='',EMBEDDER_MODEL='bekko-a25m',EMBEDDER_URL='https://aimee-embedder:8762',EMBEDDER_DIMS='384')
out=root/'evidence/managed';out.mkdir(exist_ok=True)
s=m.Stack('server',env,out);s.file='compose.server-managed.yaml';s.env['COMPOSE_PROJECT_NAME']=s.project
# Browser runner uses only the guest loopback listener.
s.network_override.write_text('services:\n  aimee-server:\n    ports: !override ["127.0.0.1:8443:8443"]\n')
(root/'private/managed-state.json').write_text(json.dumps(dict(project=s.project,application=s.application,postgres=s.postgres,embedder=s.embedder,env=s.env,file=s.file,override=str(s.network_override))))
(root/'private/managed-state.json').chmod(0o600)
m.command('python3',str(m.ROOT/'scripts/compose-vault-init.py'),*s.compose_args(),'up',env=s.env)
s.compose('up','-d','--no-build','--pull','never')
for n in range(180):
 if m.command('docker','inspect','--format','{{.State.Health.Status}}',s.application)=='healthy': break
 time.sleep(2)
else: raise RuntimeError('managed server health timeout')
logs=subprocess.check_output(['docker','logs',s.application],stderr=subprocess.STDOUT,text=True)
u=re.search(r'username: (aimee-[a-f0-9]+)',logs);p=re.search(r'password: ([a-f0-9]{64})',logs)
assert u and p,'Fresh bootstrap login missing'
(root/'private/browser.json').write_text(json.dumps(dict(url='https://127.0.0.1:8443',user=u[1],password=p[1])))
(root/'private/browser.json').chmod(0o600)
m.command('docker','cp',str(root/'aimee-client'),s.application+':/tmp/published-aimee')
m.command('docker','cp',str(root/'source/scripts/validation/providers/fixture.py'),s.application+':/tmp/provider-fixture.py')
m.command('docker','exec','-d',s.application,'python3','/tmp/provider-fixture.py','--port','18765')
(out/'deployment.json').write_text(json.dumps(dict(project=s.project,application=s.application,postgres=s.postgres,embedder=s.embedder,healthy=True),indent=2)+'\n')
print('Fresh managed browser deployment healthy; bootstrap credentials saved privately')
