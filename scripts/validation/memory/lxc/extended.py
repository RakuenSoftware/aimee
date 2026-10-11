#!/usr/bin/env python3
import ast,concurrent.futures,hashlib,json,os,signal,subprocess,time,urllib.request
from pathlib import Path
backend=os.environ['AIMEE_MEMORY_BACKEND']
root=Path('/var/lib/aimee-memory-lab');out=root/'extended-evidence';out.mkdir(exist_ok=True)
base=os.environ['SERVER_URL'];checks=[];counter=0;run_tag=str(time.time_ns());project='extended-lxc-'+run_tag
# Reuse the shipping request/response validation functions without rerunning
# the primary probe at import time.
source=ast.parse(Path('/opt/aimee/scripts/aimee-memory-lxc-probe.py').read_text())
functions=[n for n in source.body if isinstance(n,ast.FunctionDef) and n.name in ('request','require','check','args','get','has')]
exec(compile(ast.Module(body=functions,type_ignores=[]),'request_helpers','exec'))
def sql(query):
 return subprocess.check_output(['runuser','-u','postgres','--','/usr/lib/postgresql/18/bin/psql','-X','-d','aimee_validation','-tAc',query],text=True).strip()
def memory_pids():
 result=[]
 for proc in Path('/proc').iterdir():
  if not proc.name.isdigit():continue
  try:exe=str((proc/'exe').resolve());argv=(proc/'cmdline').read_bytes()
  except OSError:continue
  if exe.endswith('/aimee-module-memory') and b'module-bus.sock' in argv:result.append(int(proc.name))
 return result
if backend!='native':
 def independence():
  roles=['ct_runtime','aimee_kb_runtime','PUBLIC']
  tables=['aimee_private.user_memories','public.memories']
  saved=[]
  for table in tables:
   for role in roles:
    for privilege in ['SELECT','INSERT','UPDATE','DELETE']:
     # PUBLIC is checked by table ACL, not a nonexistent login role.
     if role=='PUBLIC':
      if sql(f"SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace CROSS JOIN LATERAL aclexplode(COALESCE(c.relacl,acldefault('r',c.relowner))) a WHERE n.nspname='{table.split('.')[0]}' AND c.relname='{table.split('.')[1]}' AND a.grantee=0 AND a.privilege_type='{privilege}'")!='0':saved.append((role,table,privilege))
      continue
     if sql(f"SELECT has_table_privilege('{role}','{table}','{privilege}')")=='t':saved.append((role,table,privilege))
  try:
   for table in tables:
    for role in roles:sql(f'REVOKE ALL ON TABLE {table} FROM {role}')
   for table in tables:
    assert sql(f"SELECT has_table_privilege('ct_runtime','{table}','SELECT')")=='f'
   for store in ['user','kb']:
    data=require(request('memory/store',args(store,key='native-denied-'+run_tag,content='canonical independently durable needle '+store,kind='fact')))
    assert has(get(store,data['id']),'independently durable')
    result=require(request('memory/search',args(store,keywords=['independently'],limit=5)))
    assert has(result,'independently durable'),result
  finally:
   for role,table,privilege in saved:sql(f'GRANT {privilege} ON TABLE {table} TO {role}')
 check('shipping CRUD/search with native record-table access revoked',independence)

 def capacity():
  def create(index):
   data=require(request('memory/store',{'store':'kb','project':project+'-capacity','key':f'capacity-{index}','content':'bulk-capacity needle '+str(index),'kind':'fact'}));return data['id']
  with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:ids=list(pool.map(create,range(257)))
  assert len(set(ids))==257
  result=request('memory/search',{'store':'kb','project':project+'-capacity','keywords':['bulk-capacity'],'limit':1})
  assert result.get('status')=='ok',result
  assert has(require(request('memory/get',{'store':'kb','project':project+'-capacity','id':ids[-1]})),'256')
 check('257 eligible records support bounded retrieval without truncating stored data',capacity)

# Discover actual authors from newly created synthetic records instead of
# accepting erasure of a subject that never authored any records.
erase_records={};backups={};subjects=set()
for store in ['user','kb']:
 def authored(store=store):
  result=require(request('memory/store',args(store,key='actual-erasure-'+run_tag,content='erase-marker-'+run_tag+' '+store,kind='fact')))
  erase_records[store]=result['id']
  if backend=='native':
   if store=='user':subject=sql(f"SELECT author_principal FROM aimee_private.user_memories WHERE id={int(result['id'])}")
   else:subject=sql(f"SELECT actor_principal FROM public.memory_fact_actors WHERE memory_id={int(result['id'])}")
   assert subject
   subjects.add(subject)
  else:
   run=Path(os.environ['RUN_ROOT']);home=run/'stack' if store=='user' else run/'kb-stack'
   path=home/'memory-backends'/backend/('server' if store=='user' else 'kb')/'records.json'
   raw=path.read_bytes();state=json.loads(raw)
   subjects.add(state['entries'][str(result['id'])]['record']['authorship']['principal'])
   backups[path]=raw
 check(store+' verified actual author discovered from durable metadata',authored)

def actual_erase():
 assert len(erase_records)==2 and subjects
 for subject in subjects:
  result=require(request('kb/erase-subject',{'subject':subject,'request_id':'actual-lxc-'+backend+'-'+run_tag+'-'+hashlib.sha256(subject.encode()).hexdigest()[:10]}))
  assert result.get('coverage_complete') is True,result
 for store,id in erase_records.items():
  if backend=='native':
   table='aimee_private.user_memories' if store=='user' else 'public.memories'
   assert sql(f'SELECT count(*) FROM {table} WHERE id={int(id)}')=='0', f'{table} retains erased record {id}'
  else:
   path=next(p for p in backups if ('/server/' in str(p))==(store=='user'))
   state=json.loads(path.read_text())
   assert str(id) not in state['entries'],state.keys()
   assert ('erase-marker-'+run_tag).encode() not in path.read_bytes()
check('actual-author erasure physically removes canonical/history/receipt content',actual_erase)

if backend!='native' and backups:
 def restore():
  victims=memory_pids();assert len(victims)==2
  for pid in victims:os.kill(pid,signal.SIGSTOP)
  try:
   for path,raw in backups.items():
    temp=path.with_name('stale-restore.json');temp.write_bytes(raw);temp.chmod(0o600);os.replace(temp,path)
  finally:
   for pid in victims:os.kill(pid,signal.SIGKILL)
  time.sleep(3)
  deadline=time.monotonic()+150
  while True:
   if len(memory_pids())==2 and all(('erase-marker-'+run_tag).encode() not in p.read_bytes() for p in backups):break
   assert time.monotonic()<deadline,'startup failed to purge retained erasure markers'
   time.sleep(.5)
  for p in backups:
   assert p.with_name('erasures.json').exists()
   assert ('erase-marker-'+run_tag).encode() not in p.read_bytes()
 check('stale canonical restore replays retained erasure metadata before readiness',restore)
else:
 def restart_erased():
  for pid in memory_pids():os.kill(pid,signal.SIGTERM)
  time.sleep(4)
  for store,id in erase_records.items():
   table='aimee_private.user_memories' if store=='user' else 'public.memories'
   assert sql(f'SELECT count(*) FROM {table} WHERE id={int(id)}')=='0', f'{table} retains erased record {id}'
 check('native memory restart preserves actual erased absence',restart_erased)

if backend!='native':
 def readmission():
  fresh={}
  for store in ['user','kb']:
   data=require(request('memory/store',args(store,key='fresh-after-erasure-'+run_tag,content='fresh-admission-'+run_tag+' '+store,kind='fact')))
   fresh[store]=data['id']
  for subject in subjects:
   result=require(request('kb/erase-subject',{'subject':subject,'request_id':'actual-lxc-'+backend+'-'+run_tag+'-'+hashlib.sha256(subject.encode()).hexdigest()[:10]}))
   assert result.get('coverage_complete') is True,result
  for store,id in fresh.items():assert has(get(store,id),'fresh-admission-'+run_tag)
 check('fresh admission after erasure and completed-erasure retry preserves new records',readmission)

# Exercise limits and diagnostics on the running Hillock API using only an
# explicit synthetic candidate snapshot. No application corpus is exported.
if backend=='hillock':
 def profile():
  token=Path('/var/lib/aimee-provider/token').read_text().strip()
  candidates=[{'id':1,'revision':'a'*64,'text':'Kibukx person height 69cm originally stated by Virant'}, {'id':2,'revision':'b'*64,'text':'Kibukx mountains elevation 69 feet reported by Virant'}]
  cases=[]
  for query in ["Kibukx real height", "Who told you Kibukx height?", "How tall is the person?", "Kibukx is not 69cm tall", "The mountain elevation", "Which colored light helps ships locate the dock after dark?"]:
   data=json.dumps({'query':query,'candidates':candidates,'limit':2}).encode()
   req=urllib.request.Request('http://127.0.0.1:8097/v1/rank',data=data,headers={'Content-Type':'application/json','Authorization':'Bearer '+token})
   with urllib.request.urlopen(req,timeout=15) as response:result=json.load(response)
   assert all(h['id'] in (1,2) for h in result['hits'])
   cases.append({'query':query,'result':result})
  (out/'hillock-language-probes.json').write_text(json.dumps(cases,indent=2))
 check('real Hillock identity/unit/negation/paraphrase diagnostic probes',profile)

(out/'summary.json').write_text(json.dumps({'backend':backend,'status':'passed' if all(c['status']=='passed' for c in checks) else 'failed','checks':checks},indent=2))
raise SystemExit(0 if all(c['status']=='passed' for c in checks) else 1)
