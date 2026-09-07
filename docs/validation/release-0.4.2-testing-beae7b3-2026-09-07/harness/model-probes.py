import json,os,subprocess,time
from pathlib import Path
p=Path('/opt/validation042');state=json.loads((p/'private/managed-state.json').read_text());images=json.loads((p/'evidence/images.json').read_text());checks=[]
def run(*args,**kw):return subprocess.run(args,check=True,text=True,capture_output=True,timeout=240,**kw)
def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));print(('PASS ' if ok else 'FAIL ')+name,flush=True)
try:
 for variant in ('E2B','E4B'):
  image=images['AIMEE_LLM_IMAGE' if variant=='E2B' else 'AIMEE_LLM_E4B_IMAGE']
  r=run('bash',str(p/'source/scripts/test-synthesis-portability.sh'),image);(p/f'evidence/synthesis-{variant}-portability.log').write_text(r.stdout+r.stderr);check(variant+' host and Nehalem CPU probes',True)
  env=dict(state['env'],AIMEE_LLM_IMAGE=image,COMPOSE_PROFILES='synthesis')
  run('docker','compose','--env-file','/dev/null','-p',state['project'],'-f',state['file'],'-f',state['override'],'up','-d','--no-deps','--no-build','--pull','never','aimee-llm',env=env,cwd=p/'source')
  container=state['project']+'-aimee-llm-1'
  ip=run('docker','inspect','--format','{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}',container).stdout.strip()
  hosts=Path('/etc/hosts');lines=[line for line in hosts.read_text().splitlines() if 'aimee-llm' not in line];hosts.write_text('\n'.join(lines)+'\n'+ip+' aimee-llm\n')
  mounts=json.loads(run('docker','inspect',state['application']).stdout)[0]['Mounts'];source=next(v['Source'] for v in mounts if v['Destination']=='/run/aimee-model-tls')
  dest=Path('/run/aimee-model-tls')
  if not dest.exists():dest.symlink_to(source,target_is_directory=True)
  for attempt in range(120):
   r=subprocess.run(['curl','--silent','--show-error','--max-time','3','--cacert',str(dest/'synthesis/client/ca.pem'),'--cert',str(dest/'synthesis/client/client.pem'),'--key',str(dest/'synthesis/client/client.key'),'https://aimee-llm:8761/v1/models'],capture_output=True,text=True)
   if r.returncode==0:
    try:
     if json.loads(r.stdout).get('data'):break
    except ValueError:pass
   time.sleep(2)
  else:raise RuntimeError(variant+' readiness timed out')
  r=run(str(p/'source/src/build/obj/tests/unit-test-synthesis-mtls-client'),'--managed-live',env=dict(os.environ,AIMEE_MODEL_SERVICES_ENABLED='1'));(p/f'evidence/synthesis-{variant}-native.log').write_text(r.stdout+r.stderr);check(variant+' real native managed mTLS discovery and inference',True)
  before=run('docker','logs',container).stderr
  r=subprocess.run(['curl','--silent','--show-error','--max-time','10','--cacert',str(dest/'synthesis/client/ca.pem'),'https://aimee-llm:8761/v1/models'],capture_output=True,text=True)
  after=run('docker','logs',container).stderr
  check(variant+' anonymous client refused with no response body',r.returncode!=0 and r.stdout=='')
  check(variant+' model remains running after rejected client',json.loads(run('docker','inspect',container).stdout)[0]['State']['Running'])
except Exception as e:
 check('model probes completed',False);(p/'private/model-probe-error.txt').write_text(str(e))
finally:(p/'evidence/model-probes.json').write_text(json.dumps(checks,indent=2)+'\n')
raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
