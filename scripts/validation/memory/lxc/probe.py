#!/usr/bin/env python3
"""Shipping API acceptance; fixture records only, no credentials in artifacts."""
import concurrent.futures, hashlib, json, os, signal, subprocess, time
from pathlib import Path
backend=os.environ['AIMEE_MEMORY_BACKEND']
root=Path('/var/lib/aimee-memory-lab');root.mkdir(parents=True,exist_ok=True)
out=root/'evidence';out.mkdir(exist_ok=True)
(root/'last-run').write_text(os.environ['RUN_ROOT'])
base=os.environ['SERVER_URL']
checks=[];counter=0
run_tag=str(time.time_ns())
project='memory-lxc-'+run_tag

def request(route, body=None, wait=True, method=None):
 global counter
 start=time.monotonic()
 command=['curl','-sS','--cacert',str(Path(os.environ['CLIENT_CERT']).parents[1]/'remote-ca.pem'),'--max-time','145','--cert',os.environ['CLIENT_CERT'],'--key',os.environ['CLIENT_KEY'],'-H','Authorization: Bearer '+os.environ['BEARER'],'-H','Content-Type: application/json']
 if body is not None:command+=['-X',method or 'POST','--data-binary',json.dumps(body)]
 command += [base+'/v1/'+route,'-w','\n%{http_code}']
 p=subprocess.run(command,capture_output=True,text=True)
 if p.returncode:raise RuntimeError(f'{route}: transport {p.returncode}: {p.stderr}')
 raw, code=p.stdout.rsplit('\n',1);value=json.loads(raw)
 if wait and value.get('object')=='op.run':
  deadline=time.monotonic()+160
  while value.get('status') in ('queued','in_progress') and time.monotonic()<deadline:
   time.sleep(.2);value=request('runs/'+value['id'],wait=False)
  assert value.get('status') in ('completed','failed'),value
  value=value.get('result',value)
 counter+=1
 (out/f'api-{counter:04d}.json').write_text(json.dumps({'route':route,'http':int(code),'response':value,'elapsed_seconds':round(time.monotonic()-start,3)},indent=2))
 return value

def require(result):
 assert result.get('status') in ('ok','completed'),result
 return result

def check(name, fn):
 start=time.monotonic()
 try:fn();result={'name':name,'status':'passed'}
 except Exception as e:result={'name':name,'status':'failed','error':str(e)[:3000]}
 result['seconds']=round(time.monotonic()-start,3);checks.append(result)
 print(result['status'].upper(),name, result.get('error',''),flush=True)
 (out/'summary.json').write_text(json.dumps({'backend':backend,'checks':checks,'status':'running'},indent=2))

def args(store, **values):
 values['store']=store
 if store=='kb':values['project']=project
 return values

def get(store,id,**values):return require(request('memory/get',args(store,id=id,**values)))
def has(result,text):return text in json.dumps(result,ensure_ascii=False)
rows={}
for store in ('user','kb'):
 deadline=time.monotonic()+180
 while True:
  ready=request('memory/list',args(store,limit=1))
  if ready.get('status')=='ok':break
  assert time.monotonic()<deadline,ready
  time.sleep(1)

def core(store):
 data=require(request('memory/store',args(store,key=f'{store}-Kibukx height-'+run_tag,content=f'needle {store}: Kibukx real height is 69cm; originally reported by Virant. fixture={run_tag}',kind='fact',tier='L2',idempotency_key='lxc-create-'+run_tag)))
 rows[store]=data['id']
 repeat=require(request('memory/store',args(store,key=f'{store}-Kibukx height-'+run_tag,content=f'needle {store}: Kibukx real height is 69cm; originally reported by Virant. fixture={run_tag}',kind='fact',tier='L2',idempotency_key='lxc-create-'+run_tag)))
 assert repeat['id']==data['id'],repeat
 record=get(store,data['id'],include_version=True)
 assert has(record,'69cm') and not has(record,'69 feet'),record
 assert has(record,'version'),record
 query=require(request('memory/search',args(store,keywords=['needle'],limit=10)))
 assert has(query,f'needle {store}:'),query
 assert not has(query,f'needle {"kb" if store=="user" else "user"}:'),query
check('private CRUD/source-unit retention/retry/retrieval/isolation',lambda:core('user'))
check('shared CRUD/source-unit retention/retry/retrieval/isolation',lambda:core('kb'))

for store in ('user','kb'):
 if store not in rows:continue
 def correction(store=store):
  before=get(store,rows[store],include_version=True)
  memory=before['memory'];version=memory['version']
  data=require(request('memory/supersede',args(store,id=rows[store],old_id=rows[store],new_content=f'needle {store}: Kibukx corrected height is 170cm; human correction. fixture={run_tag}',expected_version=version,idempotency_key='lxc-correct-'+run_tag)))
  rows[store]=data.get('new_id',data.get('id',rows[store]))
  after=get(store,rows[store],include_version=True)
  assert has(after,'170cm') and not has(after,'height is 69cm'),after
  stale=request('memory/supersede',args(store,id=int(version['record_id']),old_id=int(version['record_id']),new_content='Kibukx is 4 feet',expected_version=version))
  assert stale.get('status')=='error' and stale.get('kind') in ('conflict','version_conflict','not_found'),stale
 check(store+' correction and stale-version refusal',correction)
 if store=='user':
  def history():
   # The independent catalog preserves the same ID across revisions.
   files=sorted(out.glob('api-*.json'))
   version=None
   for f in files:
    item=json.loads(f.read_text())
    if item['route']=='memory/get' and has(item['response'],'69cm') and item['response'].get('store')=='user':
     version=item['response']['memory']['version'];break
   assert version
   result=get('user',int(version['record_id']),at_version=version)
   assert has(result,'69cm') and has(result,'historical'),result
  check('personal exact historical version',history)

if 'kb' in rows:
 def scopes():
  other=require(request('memory/search',{'store':'kb','project':'other-project','keywords':['needle'],'limit':10}))
  assert not has(other,'needle kb:'),other
 check('shared project isolation',scopes)

for store in ('user','kb'):
 def retire(store=store):
  created=require(request('memory/store',args(store,key='delete-'+store+'-'+run_tag,content='temporary deletion fixture '+run_tag,kind='fact')))
  observed=get(store,created['id'],include_version=True)
  require(request('memory/delete',args(store,id=created['id'],expected_version=observed['memory']['version'],idempotency_key='lxc-delete-'+run_tag)))
  gone=request('memory/get',args(store,id=created['id']))
  assert gone.get('kind')=='not_found',gone
 check(store+' record deletion/current exclusion',retire)

def restart_memory():
 victims=[]
 for proc in Path('/proc').iterdir():
  if not proc.name.isdigit():continue
  try:exe=str((proc/'exe').resolve());argv=(proc/'cmdline').read_bytes()
  except OSError:continue
  if exe.endswith('/aimee-module-memory') and b'module-bus.sock' in argv:victims.append(int(proc.name))
 assert len(victims)==2,victims
 for pid in victims:os.kill(pid,signal.SIGTERM)
 time.sleep(3)
 for store in ('user','kb'):
  deadline=time.monotonic()+140
  while True:
   data=request('memory/list',args(store,limit=5))
   if data.get('status')=='ok':break
   assert time.monotonic()<deadline,data
   time.sleep(.5)
  if store in rows:assert has(get(store,rows[store]),'170cm') or has(get(store,rows[store]),'69cm')
check('both supervised memory children restart with durable records',restart_memory)

# These are real shipping chatbot assembly routes. Failures remain in evidence;
# no direct-store success may substitute for these checks.
for route,body in [
 ('memory/recall',{'task_hint':'Kibukx height','limit_tokens':1800,'project':project}),

]:
 def compose(route=route,body=body):
  result=require(request(route,body));assert not has(result,'"status": "error"'),result
 check('chatbot '+route,compose)

if backend!='native':
 for verb,body in [('backend_capabilities',{}),('backend_list',{'limit':100}),('backend_export',{})]:
  def admin(verb=verb,body=body):
   result=require(request('commands/memory.'+verb,body));assert not has(result,'"status": "error"'),result
  check('operator '+verb,admin)

# Conservative unrelated-subject cleanup must retain records in both placements.
rid='lxc-unrelated-erasure-'+backend+'-'+run_tag
body={'subject':'lxc-disposable-unrelated-subject','request_id':rid}
def erasure():
 result=require(request('kb/erase-subject',body));assert result.get('coverage_complete') is True,result
 repeated=require(request('kb/erase-subject',body));assert repeated.get('coverage_complete') is True,repeated
 for store in rows:assert has(get(store,rows[store]),'Kibukx'),rows
check('subject erasure completion/retry/retained records',erasure)

def assembly():
 result=require(request('memory/read',{'task_hint':'Kibukx height','project':project},method='GET'))
 assert has(result,'context'),result
check('shipping shared context assembly',assembly)

# Provider outage is tested through deployed Aimee, not by a fabricated transport.
if backend!='native' and rows:
 service='aimee-'+backend
 def outage():
  subprocess.run(['systemctl','stop',service],check=True)
  try:
   result=request('memory/search',args('kb',keywords=['needle'],limit=10))
   assert result.get('status')=='error',result
   for store in rows:assert has(get(store,rows[store]),'Kibukx')
  finally:subprocess.run(['systemctl','start',service],check=True)
  deadline=time.monotonic()+120
  while True:
   result=request('memory/search',args('kb',keywords=['needle'],limit=10))
   if result.get('status')=='ok':break
   assert time.monotonic()<deadline,result
   time.sleep(1)
 check('provider outage reports failure; durable exact reads survive; recovery',outage)

metadata={'backend':backend,'hostname':subprocess.check_output(['hostname'],text=True).strip(),
 'binary_sha256':{p:hashlib.sha256(Path('/opt/aimee',p).read_bytes()).hexdigest() for p in ['aimee','aimee-server','aimee-kb','src/build/obj/aimee-module']},
 'postgres':subprocess.check_output(['/usr/lib/postgresql/18/bin/postgres','--version'],text=True).strip(),
 'checks':checks,'status':'passed' if all(c['status']=='passed' for c in checks) else 'failed'}
(out/'summary.json').write_text(json.dumps(metadata,indent=2))
print('RESULT',metadata['status'],sum(c['status']=='passed' for c in checks),'/',len(checks),flush=True)
raise SystemExit(0 if metadata['status']=='passed' else 1)
