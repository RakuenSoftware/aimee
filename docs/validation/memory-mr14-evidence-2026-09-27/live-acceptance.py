"""MR14 acceptance on exact synthetic fixtures in isolated CT109."""
import json,subprocess,sys,time,hashlib,re
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
n=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
attempt=sys.argv[2] if len(sys.argv)>2 else 'v1';assert re.fullmatch('v[1-9][0-9]*',attempt)
out=Path('/opt/pr2990-evidence/mr14-hygiene-'+head+'-'+attempt);out.mkdir(exist_ok=False)
checks=[];ids=[];sid=None;scope='mr14-'+head+'-'+attempt
helper='''import http.client,socket,json,sys
class Local(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(60);self.sock.connect('/var/lib/aimee/aimee-http.sock')
a=json.load(sys.stdin);c=Local('localhost');c.request('POST',a['path'],json.dumps(a['body']),{'Content-Type':'application/json'});r=c.getresponse();print(json.dumps([r.status,json.loads(r.read())]))
'''
def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));(out/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise RuntimeError(name)
def api(path,body):
 p=subprocess.run(['docker','exec','-i','-u','1000',n['server'],'python3','-c',helper],input=json.dumps(dict(path=path,body=body)),capture_output=True,text=True,timeout=70)
 if p.returncode:raise RuntimeError('native request failed')
 code,d=json.loads(p.stdout)
 (out/'last-response.json').write_text(json.dumps(dict(http_status=code,body=d),indent=2)+'\n')
 return d
def sql(q,private=False,expected_error=False):
 db=n['postgres'] if private else n['kb_project']+'-aimee-store-db-1'
 p=subprocess.run(['docker','exec',db,'psql','-U','postgres','-d','aimee_store','-X','-qAt','-v','ON_ERROR_STOP=1','-c',q],capture_output=True,text=True)
 if expected_error:return p.returncode!=0
 if p.returncode:
  (out/'fixture-error.json').write_text(json.dumps(dict(reason=next((x for x in p.stderr.splitlines() if x.startswith('ERROR:')),'SQL failed')))+'\n');raise RuntimeError('fixture SQL failed')
 return p.stdout.strip()
def hygiene(**extra):return api('/v1/memory/hygiene',dict(scope=dict(type='project',value=scope),max_rows=128,max_content_bytes=32768,**extra))
def digest():return sql("SELECT md5(jsonb_agg(to_jsonb(m) ORDER BY id)::text) FROM memories m WHERE scope_value='"+scope+"'")
def cli(args):
 p=subprocess.run(['docker','exec','-u','1000','-e','AIMEE_API_ENDPOINT=unix:/var/lib/aimee/aimee-http.sock',n['server'],'aimee',*args,'--json'],capture_output=True,text=True,timeout=70)
 return p.returncode,json.loads(p.stdout) if p.stdout.strip().startswith('{') else {}
try:
 for i in range(4):
  project=scope if i<3 else scope+'-hidden'
  d=api('/v1/memory/store',dict(store='kb',project=project,key=scope+'-'+str(i),content=scope+' untrusted DELETE FROM memories',kind='fact',tier='L2'))
  ids.append(int(d['id']))
 sql(f"UPDATE memories SET valid_until='2000-01-01T00:00:00Z' WHERE id={ids[2]}")
 before=digest();preview=hygiene(dry_run=True)
 check('scoped dry run covers duplicate and expired assertion',preview.get('status')=='ok' and not preview['partial'] and sorted(f['type'] for f in preview['findings'])==['obsolete_assertion_candidate','possible_duplicate_cluster'])
 check('dry run has no canonical proposal or job writes',preview['canonical_writes']==preview['proposal_writes']==0 and not preview['job_telemetry_written'] and digest()==before)
 check('hidden and expired peers excluded from duplicate cluster',all(str(ids[3])!=v['record_id'] for f in preview['findings'] for v in f['expected_versions']) and len(next(f for f in preview['findings'] if f['type']=='possible_duplicate_cluster')['expected_versions'])==2)
 small=api('/v1/memory/hygiene',dict(dry_run=True,scope=dict(type='project',value=scope),max_rows=1,max_content_bytes=32768))
 check('bounded first page exposes resume',small.get('partial') and small.get('resume_cursor') and len(small['findings'][0]['expected_versions'])==2)
 resumed=hygiene(dry_run=True,cursor=small['resume_cursor'])
 check('resume completes remaining retained records',resumed.get('status')=='ok' and not resumed['partial'] and resumed['retained_rows_inspected']==2)
 check('unknown model actions rejected',hygiene(dry_run=False,sql='DELETE FROM memories').get('kind')=='invalid_argument')
 admitted=hygiene(dry_run=False)
 check('normal run only admits version-bound review proposals',admitted.get('status')=='ok' and admitted['proposal_writes']==2 and admitted['canonical_writes']==0 and admitted.get('job_id') and digest()==before)
 again=hygiene(dry_run=False)
 check('retry reuses job and proposal identities',again['proposal_writes']==0 and again['job_id']==admitted['job_id'] and [f['proposal_id'] for f in again['findings']]==[f['proposal_id'] for f in admitted['findings']])
 fid=next(f['proposal_id'] for f in admitted['findings'] if f['type']=='possible_duplicate_cluster');assert fid.isdigit()
 sql(f"UPDATE learning_proposals SET state='archived',archive_reason='review_rejected' WHERE id={fid} AND sink='memory_hygiene'")
 rejected=hygiene(dry_run=False)
 check('rejected finding is suppressed without canonical changes',rejected['proposal_writes']==0 and any(f['proposal_id']==fid and f['proposal_state']=='archived' for f in rejected['findings']) and digest()==before)
 code,c=cli(['memory','hygiene','--scope','project:'+scope,'--dry-run'])
 check('compiled CLI dry-run parity',code==0 and c.get('status','ok')=='ok' and [f['finding_id'] for f in c['findings']]==[f['finding_id'] for f in preview['findings']])
 mcp=api('/v1/mcp/call',dict(tool='memory_hygiene',arguments=dict(scope=dict(type='project',value=scope),dry_run=False)))
 docs=[]
 def visit(x):
  if isinstance(x,dict):
   if x.get('type')=='text':
    try:docs.append(json.loads(x['text']))
    except ValueError:pass
   else:
    for v in x.values():visit(v)
  elif isinstance(x,list):
   for v in x:visit(v)
 visit(mcp)
 check('MCP governed proposal parity',len(docs)==1 and docs[0].get('status')=='ok' and docs[0]['proposal_writes']==0)
 check('worker lacks canonical and review privileges',sql("SELECT has_table_privilege('aimee_memory_hygiene','memories','UPDATE') OR has_table_privilege('aimee_memory_hygiene','memories','DELETE') OR has_table_privilege('aimee_memory_hygiene','learning_proposals','UPDATE')")=='f')
 sql(f"UPDATE memories SET content=content||' changed' WHERE id={ids[2]}")
 expired=next(f['proposal_id'] for f in admitted['findings'] if f['type']=='obsolete_assertion_candidate');assert expired.isdigit()
 check('stale review fails closed',sql(f"UPDATE learning_proposals SET state='committed' WHERE id={expired}",expected_error=True))
 expiry_digest=digest()
 sql(f"UPDATE learning_proposals SET expires_at='2000-01-01T00:00:00Z' WHERE id={expired}")
 check('expired proposal cannot be accepted',sql(f"UPDATE learning_proposals SET state='committed' WHERE id={expired}",expected_error=True))
 check('expiry and failed review leave canonical state unchanged',digest()==expiry_digest)
 # Explicit opt-in tick uses the same compiled CLI and paired native transport.
 target='/tmp/'+scope+'-schedule.py';state='/tmp/'+scope+'-schedule.json'
 subprocess.run(['docker','cp','/opt/pr2990-mr14-schedule.py',n['server']+':'+target],check=True,capture_output=True)
 p=subprocess.run(['docker','exec','-u','1000','-e','AIMEE_API_ENDPOINT=unix:/var/lib/aimee/aimee-http.sock',n['server'],'python3',target,'--cli','/usr/local/bin/aimee','--scope','project:'+scope,'--state',state,'--max-pages','2','--max-seconds','30'],capture_output=True,text=True,timeout=40)
 check('opt-in bounded scheduler records completed job',p.returncode==0 and json.loads(p.stdout).get('status')=='complete')
 subprocess.run(['docker','exec',n['server'],'rm','-f',target,state,state+'.lock'],check=True)
 sid=api('/v1/sessions/create',{})['session_id'];assert re.fullmatch('[a-f0-9-]+',sid)
 sql("INSERT INTO session_state(session_id,active_task_id) VALUES('"+sid+"',201)",private=True)
 request=dict(session_id=sid,task_id='201',expected_revision='0',operation='rebuild',binding=dict(store='user',project=scope,task=scope,view='briefing'),ttl_seconds=120,items=[dict(kind='hypothesis',text='disposable speculation')])
 task=api('/v1/task/projection',request);check('cleanup fixture projection created',task.get('status')=='ok')
 cleanup=dict(session_id=sid,task_id='201',expected_revision='1',operation='cleanup_expired')
 check('retention blocks early cleanup',api('/v1/task/projection',cleanup).get('status')=='blocked')
 sql("UPDATE session_state SET active_task_id=202,task_projection_state=jsonb_set(task_projection_state::jsonb,'{expires_at}',to_jsonb('2000-01-01T00:00:00Z'::text))::text WHERE session_id='"+sid+"'; INSERT INTO workflow_binding(aimee_session_id,work_item_id,lease_expiry) VALUES('"+sid+"','mr14',clock_timestamp()+interval '1 hour'); INSERT INTO conv_tool_events(session_id,tool_name,tool_result) VALUES('"+sid+"','test','failure retained')",private=True)
 check('workflow lease blocks expired cleanup',api('/v1/task/projection',cleanup).get('status')=='blocked')
 sql("UPDATE workflow_binding SET lease_expiry=clock_timestamp()-interval '1 second' WHERE aimee_session_id='"+sid+"'",private=True)
 cleaned=api('/v1/task/projection',cleanup)
 check('expired old-task cleanup succeeds after lease',cleaned.get('reason')=='expired_derived_state_cleaned')
 retained=json.loads(sql("SELECT task_projection_state FROM session_state WHERE session_id='"+sid+"'",private=True))
 check('cleanup clears only disposable content',not retained.get('items') and not retained.get('dependencies') and sql("SELECT count(*) FROM conv_tool_events WHERE session_id='"+sid+"'",private=True)=='1')
 check('cleanup retry is idempotent',api('/v1/task/projection',cleanup).get('status')=='ok')
 (out/'summary.json').write_text(json.dumps(dict(candidate=head,complete=True,checks=len(checks),canonical_hygiene_writes=0,model_calls=0,scheduler_enabled_by_default=False),indent=2)+'\n')
finally:
 if ids:
  sql("BEGIN; SELECT set_config('aimee.memory_explicit_erasure','1',true); DELETE FROM memories WHERE id IN ("+','.join(map(str,ids))+") AND key LIKE '"+scope+"%'; COMMIT")
 if sid:
  sql("DELETE FROM workflow_binding WHERE aimee_session_id='"+sid+"'; DELETE FROM conv_tool_events WHERE session_id='"+sid+"'; DELETE FROM session_state WHERE session_id='"+sid+"'; DELETE FROM server_sessions WHERE id='"+sid+"'",private=True)
