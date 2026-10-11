#!/usr/bin/env python3
import ast,hashlib,json,os,subprocess,sys,time
from pathlib import Path
backend=sys.argv[1];phase=sys.argv[2];root=Path('/var/lib/aimee-memory-lab');out=root/('qualification-'+phase);out.mkdir(exist_ok=True)
run=Path((root/'last-run').read_text().strip());client=run/'stack/client';remote=(client/'remote.conf').read_text().splitlines()
os.environ.update(SERVER_URL=remote[0],BEARER=remote[1],CLIENT_CERT=str(client/'tls/client.crt'),CLIENT_KEY=str(client/'tls/client.key'))
base=remote[0];checks=[];counter=0;project='qualification-'+str(time.time_ns())
source=ast.parse(Path('/opt/aimee/scripts/aimee-memory-lxc-probe.py').read_text());exec(compile(ast.Module(body=[n for n in source.body if isinstance(n,ast.FunctionDef) and n.name in ('request','require','check','args','get','has')],type_ignores=[]),'helpers','exec'))
if phase=='guest-reboot':
 deadline=time.monotonic()+180
 while True:
  try:
   if all(request('memory/list',args(store,limit=1)).get('status')=='ok' for store in ['user','kb']):break
  except Exception:pass
  if time.monotonic()>deadline:break
  time.sleep(2)
check('enrolled mTLS health after '+phase,lambda:require(request('health')))
for store in ['user','kb']:
 check(store+' selected-backend list after '+phase,lambda store=store:require(request('memory/list',args(store,limit=2))))
if backend!='native' and phase=='service-resume':
 check('new authorized memory after subject erasure',lambda:require(request('memory/store',args('kb',key='post-erasure',content='New consent after completed erasure',kind='fact'))))
state=json.loads((root/'resume-private.json').read_text());token=state['kb-supervisor']['env'].get('AIMEE_KB_API_BEARER_TOKEN','')
if phase=='service-resume':
 r=subprocess.run(['curl','-sS','--max-time','150','-H','Authorization: Bearer '+token,'-H','Content-Type: application/json','--data-binary',json.dumps({'project':project,'query':'Kibukx height','limit':10}),'http://127.0.0.1:8741/v1/actions/memory.briefing','-w','\n%{http_code}'],capture_output=True,text=True)
 try:body,code=r.stdout.rsplit('\n',1);result=json.loads(body)
 except Exception:result={'transport_exit':r.returncode,'parse_error':True};code=0
 (out/'kb-briefing.json').write_text(json.dumps({'route':'/v1/actions/memory.briefing','http':int(code),'response':result},indent=2))
 checks.append({'name':'shipping KB briefing','status':'passed' if str(code)=='200' and result.get('status')=='ok' else 'failed','response':result})
files={}
for p in (run/'stack').glob('memory-backends/**/erasures.json'):files[str(p.relative_to(run))]=hashlib.sha256(p.read_bytes()).hexdigest()
for p in (run/'kb-stack').glob('memory-backends/**/erasures.json'):files[str(p.relative_to(run))]=hashlib.sha256(p.read_bytes()).hexdigest()
if phase=='service-resume':(root/'preboot-erasure-hashes.json').write_text(json.dumps(files))
else:
 before=json.loads((root/'preboot-erasure-hashes.json').read_text());checks.append({'name':'guest reboot retains erasure controls','status':'passed' if files==before else 'failed'})
(out/'summary.json').write_text(json.dumps({'backend':backend,'phase':phase,'checks':checks},indent=2))
print(json.dumps({'backend':backend,'phase':phase,'checks':checks}))

raise SystemExit(0 if all(c['status']=='passed' for c in checks) else 1)
