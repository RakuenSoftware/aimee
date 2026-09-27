"""Read/write only exact synthetic MR12 fixtures in the isolated CT109 owners."""
import json,subprocess,sys,time,hashlib,re
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
n=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
out=Path('/opt/pr2990-evidence/mr12-views-'+head+'-v1');out.mkdir(exist_ok=False)
checks=[];observations=[];ids={'user':[],'kb':[]}
scope='mr12-'+head+'-v1'
helper='''import http.client,socket,json,sys
class Local(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(60);self.sock.connect('/var/lib/aimee/aimee-http.sock')
a=json.load(sys.stdin);c=Local('localhost');c.request('POST',a['path'],json.dumps(a['body']),{'Content-Type':'application/json'});r=c.getresponse();print(json.dumps([r.status,json.loads(r.read())]))
'''
def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));(out/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise RuntimeError(name)
def api(verb,body):
 path='/v1/mcp/call' if verb=='mcp' else '/v1/memory/'+verb.replace('_','-')
 p=subprocess.run(['docker','exec','-i','-u','1000',n['server'],'python3','-c',helper],input=json.dumps(dict(path=path,body=body)),capture_output=True,text=True,timeout=70)
 if p.returncode:raise RuntimeError('native request failed')
 status,data=json.loads(p.stdout)
 observations.append(dict(verb=verb,http_status=status,status=data.get('status'),kind=data.get('kind'),response_sha256=hashlib.sha256(p.stdout.encode()).hexdigest()))
 (out/'observations.json').write_text(json.dumps(observations,indent=2)+'\n')
 if status!=200 or (verb!='mcp' and data.get('status') not in ['ok','degraded']):raise RuntimeError('memory action failed: '+verb+' '+str(status)+' '+str(data.get('kind')))
 return data
def dbname(store):return n['project' if store=='user' else 'kb_project']+'-aimee-store-db-1'
def sql(store,q):
 p=subprocess.run(['docker','exec',dbname(store),'psql','-U','postgres','-d','aimee_store','-X','-qAt','-v','ON_ERROR_STOP=1','-c',q],capture_output=True,text=True)
 if p.returncode:
  reason=next((x for x in p.stderr.splitlines() if x.startswith('ERROR:')),'SQL failed')
  (out/'sql-error.json').write_text(json.dumps(dict(reason=reason))+'\n');raise RuntimeError('fixture SQL failed')
 return p.stdout.strip()
def put(store,key,content,kind='constraint',project=scope):
 d=api('store',dict(store=store,key=key,content=content,kind=kind,tier='L2',project=project));identity=int(d['id']);ids[store].append(identity);return identity
def serve(store,view='active_constraints',**extra):
 d=api('serve',dict(store=store,view=view,task=scope,project=scope,**extra))
 rendered=d['rendered_context'];receipt=d['receipt']
 check(store+' '+view+' exact accounting',d['context_accounting']['rendered_bytes']==len(rendered.encode()) and receipt['payload_bytes']==len(rendered.encode()) and receipt['payload_sha256']=='sha256:'+hashlib.sha256(rendered.encode()).hexdigest())
 return d
def contains(d,marker):return marker in d['rendered_context']
try:
 for store in ['user','kb']:
  marker=scope+'-'+store+'-constraint'
  before=serve(store)
  mid=put(store,marker,marker+' must be reviewed')
  got=serve(store);check(store+' new constraint invalidates view',contains(got,marker) and got['cache']['identity']!=before['cache']['identity'])
  repeated=serve(store);check(store+' cache reuse issues fresh receipt',repeated['cache']['state']=='projection_reused' and repeated['receipt']['invocation_id']!=got['receipt']['invocation_id'])
  narrow=serve(store,context_limits=dict(schema_version=1,max_context_bytes=0));check(store+' zero budget retains no text',narrow['rendered_context']=='' and narrow['cache']['identity']!=got['cache']['identity'])
  card=api('claim_card',dict(store=store,id=str(mid),project=scope,expand_evidence=True))
  check(store+' card binds correction and uncertainty',card['correction']['expected_version']==card['source_version'] and card['confidence']['calibration_state']=='unknown' and card.get('expansion_state') in ['complete','partial','bounded','unavailable'])
  newmarker=marker+'-corrected'
  replaced=api('supersede',dict(store=store,old_id=str(mid),new_content=newmarker,expected_version=card['source_version'],project=scope))
  # Different public placements use different envelopes for the replacement.
  replacement=replaced.get('id') or replaced.get('memory',{}).get('id')
  if replacement and int(replacement) not in ids[store]:ids[store].append(int(replacement))
  updated=serve(store);check(store+' correction recomputes view',contains(updated,newmarker) and updated['cache']['identity']!=got['cache']['identity'])
  env={'AIMEE_API_ENDPOINT':'unix:/var/lib/aimee/aimee-http.sock'}
  cli=subprocess.run(['docker','exec','-u','1000','-e','AIMEE_API_ENDPOINT='+env['AIMEE_API_ENDPOINT'],n['server'],'aimee','memory','serve','active_constraints','--task',scope,'--store',store,'--project',scope,'--json'],capture_output=True,text=True,timeout=70)
  if cli.returncode:raise RuntimeError('native CLI served view failed')
  cli_result=json.loads(cli.stdout)
  check(store+' CLI and HTTP exact projection parity',cli_result.get('rendered_context')==updated['rendered_context'])
  mcp=api('mcp',dict(tool='memory_serve',arguments=dict(store=store,view='active_constraints',task=scope,project=scope)))
  documents=[]
  def visit(value):
   if isinstance(value,dict):
    if value.get('type')=='text' and isinstance(value.get('text'),str):
     try:documents.append(json.loads(value['text']))
     except ValueError:pass
    else:
     for child in value.values():visit(child)
   elif isinstance(value,list):
    for child in value:visit(child)
  visit(mcp)
  check(store+' MCP and HTTP exact projection parity',len(documents)==1 and documents[0].get('rendered_context')==updated['rendered_context'])

 # An empty contradiction dependency changes on insertion without editing its prior inputs.
 empty=serve('kb','open_contradictions')
 a=put('kb',scope+'-conflict-a',scope+' alpha','fact');b=put('kb',scope+'-conflict-b',scope+' beta','fact');hidden=put('kb',scope+'-hidden',scope+' hidden side','fact',scope+'-hidden')
 sql('kb',f"INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES({a},{b},pg_now_text()),({a},{hidden},pg_now_text())")
 conflicts=serve('kb','open_contradictions');check('contradictions include both visible sides and no hidden side',contains(conflicts,scope+' alpha') and contains(conflicts,scope+' beta') and not contains(conflicts,'hidden side') and conflicts['cache']['identity']!=empty['cache']['identity'])
 sql('kb',f"UPDATE memories SET lifecycle_state='revoked' WHERE id={b}")
 revoked=serve('kb','open_contradictions');check('revocation removes the whole contradiction bundle',not contains(revoked,scope+' alpha') and not contains(revoked,scope+' beta'))
 future=put('kb',scope+'-future',scope+' future constraint')
 sql('kb',f"UPDATE memories SET valid_from=(CURRENT_TIMESTAMP+interval '8 seconds')::text WHERE id={future}")
 revision=sql('kb',f'SELECT record_revision FROM memories WHERE id={future}')
 before=serve('kb');check('future constraint withheld before boundary',not contains(before,scope+' future constraint'))
 deadline=time.monotonic()+15
 while time.monotonic()<deadline:
  after=serve('kb')
  if contains(after,scope+' future constraint'):break
  time.sleep(1)
 check('clock alone refreshes cached current view',contains(after,scope+' future constraint') and after['cache']['identity']!=before['cache']['identity'] and revision==sql('kb',f'SELECT record_revision FROM memories WHERE id={future}'))
 h1=serve('kb','historical_context',valid_at='2025-01-01T00:00:00Z');h2=serve('kb','historical_context',valid_at='2025-02-01T00:00:00Z')
 check('explicit historical coordinates bind separate cache identities',h1['historical'] and h2['historical'] and h1['cache']['identity']!=h2['cache']['identity'])
 (out/'summary.json').write_text(json.dumps(dict(complete=True,checks=len(checks),candidate=head,optional_policies_promoted=False),indent=2)+'\n')
 print('MR12_VIEWS_ACCEPTANCE_COMPLETE',len(checks),flush=True)
finally:
 # Only exact IDs created above, additionally bound to the unique fixture key prefix.
 for store,owned in ids.items():
  if not owned:continue
  table='user_memories' if store=='user' else 'memories'
  sql(store,"BEGIN; SELECT set_config('aimee.memory_explicit_erasure','1',true); DELETE FROM "+table+" WHERE id IN ("+','.join(map(str,owned))+") AND key LIKE '"+scope+"%'; COMMIT")
