import concurrent.futures,importlib.util,json,subprocess,time
from pathlib import Path
from types import SimpleNamespace
p=Path('/opt/validation042');state=json.loads((p/'private/managed-state.json').read_text())
spec=importlib.util.spec_from_file_location('placement',p/'source/tests/e2e/memory-placement-e2e.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
g=m.Gate(SimpleNamespace(server=state['application'],store_db=state['postgres']))
checks=[];observations=[]
def check(name,ok,detail=None):
 checks.append(dict(name=name,passed=bool(ok),detail=detail));print(('PASS ' if ok else 'FAIL ')+name,flush=True)
def cli(*args):
 r=subprocess.run(['docker','exec','-u','1000','-e','AIMEE_API_ENDPOINT=unix:/var/lib/aimee/aimee-http.sock',state['application'],'/tmp/published-aimee','--json',*args],capture_output=True,text=True,timeout=60)
 try:b=json.loads(r.stdout)
 except ValueError:b={}
 return r.returncode,b
try:
 rc,b=cli('memory','store','release042-cli-canary','CLI Unicode 🦊 αβ 日本語\nsecond line')
 check('published CLI stores a Unicode multiline memory',rc==0 and isinstance(b.get('id'),int));mid=b.get('id')
 if mid:
  rc,b=cli('memory','get',str(mid));check('published CLI reads exact content by returned ID',rc==0 and b.get('memory',{}).get('content')=='CLI Unicode 🦊 αβ 日本語\nsecond line')
  rc,b=cli('memory','supersede',str(mid),'replacement CLI content');check('published CLI supersedes its own returned ID',rc==0 and isinstance(b.get('id'),int) and b.get('content')=='replacement CLI content')
  replacement_id=b.get('id')
  if isinstance(replacement_id,int):
   rc,replacement=cli('memory','get',str(replacement_id));check('published CLI reads exact superseding content by replacement ID',rc==0 and replacement.get('memory',{}).get('content')=='replacement CLI content')
  rc,b=cli('memory','get',str(mid));observations.append(dict(name='get original superseded ID',exit=rc,response=b))
 contents=['Concurrent fixture '+str(i)+' 🦊 café 日本語\nline 2' for i in range(24)]
 def store(i):return g.call('store',dict(key='release042-concurrent-'+str(i),content=contents[i],confidence=0.25))
 with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool: replies=list(pool.map(store,range(24)))
 ids=[b.get('id') for _,b in replies]
 check('24 concurrent writes all succeed with distinct IDs',all(code==200 and b.get('status')=='ok' for code,b in replies) and None not in ids and len(set(ids))==24)
 for i,mid in enumerate(ids):
  if mid is None:continue
  code,b=g.call('get',dict(id=mid));check(f'concurrent fixture {i} preserves Unicode and confidence',code==200 and b.get('memory',{}).get('content')==contents[i] and b.get('memory',{}).get('confidence')==0.25)
 for confidence in [-1,2,False,True,None,'0.25','NaN',[],{},1e300]:
  code,b=g.call('store',dict(key='release042-malformed-confidence',content='invalid-confidence fixture',confidence=confidence));check('browser-enabled server rejects confidence '+repr(confidence),code==400 and b.get('kind')=='invalid_argument',dict(http=code,kind=b.get('kind')))
 for label,payload in [('missing content',{'key':'release042-empty'}),('object content',{'key':'release042-empty','content':{}}),('array content',{'key':'release042-empty','content':[]}),('null content',{'key':'release042-empty','content':None})]:
  code,b=g.call('store',payload);check('memory store rejects '+label,code==400 and b.get('status')=='error',dict(http=code,kind=b.get('kind')))
 for mid in (-1,0,4294967297,9007199254740991):
  code,b=g.call('get',dict(id=mid));check('missing/invalid ID cannot read a different record '+str(mid),b.get('status')=='error' and not b.get('memory'),dict(http=code,kind=b.get('kind')))
 for endpoint in ('/v1/health','/v1/ready','/v1/cli/manifest','/v1/status'):
  code,b=g.call(endpoint,method='GET');observations.append(dict(name=endpoint,http=code,keys=list(b) if isinstance(b,dict) else []))
 for payload in (dict(store='invalid'),dict(store='kb')):
  code,b=g.call('list',payload);check('explicit unavailable store does not return personal success '+str(payload),code>=400 and b.get('status')=='error',dict(http=code,kind=b.get('kind')))
 g.docker('restart',state['application']);g.wait('list')
 for i,mid in enumerate(ids):
  if mid is None:continue
  code,b=g.call('get',dict(id=mid));check(f'concurrent fixture {i} survives application restart',code==200 and b.get('memory',{}).get('content')==contents[i])
 g.docker('stop',state['postgres'])
 try:
  for op,body in [('store',dict(key='release042-offline',content='offline synthetic fixture')),('list',{}),('get',dict(id=ids[0])),('recall',dict(query='release042'))]:
   code,b=g.call(op,body);check('database outage produces explicit failure for '+op,code>=500 and b.get('status')=='error',dict(http=code,kind=b.get('kind')))
 finally:g.docker('start',state['postgres'])
 g.wait('get',dict(id=ids[0]));check('database outage recovers exact persisted content',g.call('get',dict(id=ids[0]))[1].get('memory',{}).get('content')==contents[0])
 for mid in ids:
  if mid is not None:g.call('delete',dict(id=mid))
 check('retired concurrency fixture is no longer readable',g.call('get',dict(id=ids[0]))[1].get('status')=='error')
except Exception as e:check('exploratory suite completed',False,type(e).__name__+': '+str(e))
finally:
 (p/'evidence/exploratory.json').write_text(json.dumps(dict(checks=checks,observations=observations),indent=2,ensure_ascii=False)+'\n')
raise SystemExit(0 if all(c['passed'] for c in checks) else 1)
