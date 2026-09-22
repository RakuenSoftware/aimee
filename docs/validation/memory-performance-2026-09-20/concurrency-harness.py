# Retained one-off measurement harness for the owned CT 9498 fixtures.
# Fixture credentials are read/generated at runtime and never enter receipts.
# Requires the paired stacks and private fixture credential file in that guest.
import concurrent.futures,datetime,http.client,json,math,pathlib,socket,statistics,subprocess,time,secrets
root=pathlib.Path('/opt/aimee-2983-evidence/performance-bddab521be');p=root/'paired/paired-retrieval.json'
while not p.exists():time.sleep(2)
paired=json.loads(p.read_text());assert paired['complete']
previous=json.loads(pathlib.Path('/opt/aimee-2983-evidence/profile-private.json').read_text())['candidate']
project=paired['arms']['candidate']['project'];opt=dict(application=project+'-aimee-kb-1',token='scope:service:aimee-server:'+secrets.token_hex(32))
arms={'previous_go':previous,'optimized':opt};corpus=json.loads(pathlib.Path('/opt/aimee-memory-perf/tests/eval/memory_retrieval_corpus.json').read_text())
old=json.loads(pathlib.Path('/opt/aimee-2983-evidence/paired-release/paired-retrieval.json').read_text());expected={r['id']:r['retrieved'] for r in old['arms']['candidate']['runs'][1]['cases']}
def docker(*a,**kw):return subprocess.run(['docker',*a],check=True,text=True,capture_output=True,**kw).stdout
for label,a in arms.items():
 a['project']=a['application'].removesuffix('-aimee-kb-1');a['containers']=[a['application'],a['project']+'-aimee-store-db-1',a['project']+'-aimee-embedder-1'];docker('start',*a['containers'])
docker('exec','-i','-u','1000','-e','AIMEE_VAULT_ENV_OVERWRITE=1',opt['application'],'aimee-kb','--bootstrap-vault-stdin',input='AIMEE_KB_API_BEARER_TOKEN='+opt['token']+'\0');docker('restart',opt['application'])
for a in arms.values():
 deadline=time.monotonic()+180
 while docker('inspect','--format','{{.State.Health.Status}}',a['application']).strip()!='healthy':
  if time.monotonic()>deadline:raise RuntimeError('load fixture readiness')
  time.sleep(2)
def usage(names):
 out={}
 for name in names:
  c=http.client.HTTPConnection('localhost');c.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);c.sock.connect('/var/run/docker.sock');c.request('GET','/containers/'+name+'/stats?stream=false&one-shot=true');r=c.getresponse();d=json.loads(r.read());c.close();assert r.status==200;out[name]=d['cpu_stats']['cpu_usage']['total_usage']
 return out
names=[name for a in arms.values() for name in a['containers']]
for name in names:
 deadline=time.monotonic()+180
 while docker('inspect','--format','{{if .State.Health}}{{.State.Health.Status}}{{else}}healthy{{end}}',name).strip()!='healthy':
  if time.monotonic()>deadline:raise RuntimeError('provider readiness timeout')
  time.sleep(2)
time.sleep(30) # exclude process/model startup from idle measurement
before=usage(names);start=time.monotonic();time.sleep(30);after=usage(names);elapsed=time.monotonic()-start
out={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'idle_seconds':elapsed,'idle_cpu_cores':{k:(after[k]-v)/1e9/elapsed for k,v in before.items()},'arms':{}}
code='''import json,sys,http.client,time,concurrent.futures
a=json.load(sys.stdin)
def query(case):
 c=http.client.HTTPConnection('127.0.0.1',8741,timeout=70);start=time.perf_counter();c.request('POST','/v1/actions/memory.find_facts',json.dumps(dict(query=case['query'],limit=20,scope='all')),{'Content-Type':'application/json','Authorization':'Bearer '+a['token']});r=c.getresponse();d=json.loads(r.read());c.close();assert r.status==200 and isinstance(d.get('facts'),list);return dict(case=case['id'],ms=(time.perf_counter()-start)*1000,ids=[x['id'] for x in d['facts']])
for case in a['cases'][:10]:query(case)
start=time.perf_counter()
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:rows=list(pool.map(query,a['cases']))
print(json.dumps(dict(elapsed_seconds=time.perf_counter()-start,rows=rows)))
'''
bykey={x['key']:x['fid'] for x in corpus['fixtures']}
for label,a in arms.items():
 ids={str(x['id']):bykey[x['key']] for x in json.loads(docker('exec',a['project']+'-aimee-store-db-1','psql','-U','postgres','-d','aimee_store','-At','-c',"SELECT json_agg(json_build_object('id',id,'key',key)) FROM memories"))}
 before=usage(a['containers']);data=json.loads(docker('exec','-i',a['application'],'python3','-c',code,input=json.dumps(dict(token=a['token'],cases=corpus['cases']))));after=usage(a['containers'])
 for row in data['rows']:
  row['retrieved']=[ids[str(x)] for x in row.pop('ids')];row['unchanged']=row['retrieved']==expected[row['case']]
 assert all(row['unchanged'] for row in data['rows'])
 vals=[r['ms'] for r in data['rows']];data['summary']=dict(concurrency=8,samples=len(vals),p50_ms=statistics.median(vals),p95_ms=sorted(vals)[math.ceil(len(vals)*.95)-1],queries_per_second=len(vals)/data['elapsed_seconds'],cpu_seconds_including_10_warmups={k:(after[k]-v)/1e9 for k,v in before.items()});out['arms'][label]=data;print(label,json.dumps(data['summary']),flush=True)
(root/'concurrency-and-idle.json').write_text(json.dumps(out,indent=2)+'\n')
for a in arms.values():docker('stop',*a['containers'])
print('All concurrent rankings unchanged; idle/CPU measurements retained; tested stacks stopped',flush=True)
