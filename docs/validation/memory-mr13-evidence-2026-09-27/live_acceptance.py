"""Exact synthetic MR13 acceptance fixtures, isolated CT109 only."""
import json,subprocess,sys,time,hashlib,re,concurrent.futures
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
n=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
out=Path('/opt/pr2990-evidence/mr13-tasks-'+head+'-v1');out.mkdir(exist_ok=False)
checks=[];ids=[];sessions=[];scope='mr13-'+head+'-v1'
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
 if p.returncode:raise RuntimeError('native transport failed')
 code,d=json.loads(p.stdout)
 if code not in (200,400,401,403,404,409,503):raise RuntimeError('unexpected HTTP '+str(code))
 return d
def sql(q):
 p=subprocess.run(['docker','exec',n['postgres'],'psql','-U','postgres','-d','aimee_store','-X','-qAt','-v','ON_ERROR_STOP=1','-c',q],capture_output=True,text=True)
 if p.returncode:
  (out/'fixture-error.json').write_text(json.dumps(dict(reason=next((x for x in p.stderr.splitlines() if x.startswith('ERROR:')),'SQL failed')))+'\n');raise RuntimeError('fixture SQL failed')
 return p.stdout.strip()
def mem(verb,**body):return api('/v1/memory/'+verb,dict(store='user',project=scope,**body))
def task(op,**args):return api('/v1/task/projection',dict(operation=op,session_id=sid,task_id='101',**args))
def rebuild(rev,**extra):return task('rebuild',expected_revision=str(rev),binding=dict(store='user',project=scope,task=scope,view='briefing'),ttl_seconds=extra.pop('ttl_seconds',120),items=[dict(kind='hypothesis',text=scope+' condition may apply'),dict(kind='planned_action',text='review the condition')],**extra)
def account(d):
 r=d.get('rendered_context','');p=d.get('receipt',{})
 return p.get('payload_bytes')==len(r.encode()) and p.get('payload_sha256')=='sha256:'+hashlib.sha256(r.encode()).hexdigest()
try:
 sid=api('/v1/sessions/create',{})['session_id'];assert re.fullmatch('[a-f0-9-]+',sid);sessions.append(sid)
 sql("INSERT INTO session_state(session_id,active_task_id) VALUES('"+sid+"',101)")
 initial=mem('store',key=scope+'-target',content=scope+' initial condition',kind='constraint',tier='L2');mid=int(initial['id']);ids.append(mid)
 before=sql(f'SELECT content FROM user_memories WHERE id={mid}')
 created=rebuild(0,prepared=dict(forged=True),authoritative=True)
 check('rebuild binds derived revision and fresh evidence',created.get('status')=='ok' and created['projection']['revision']=='1' and created['projection']['class']=='derived_non_authoritative' and scope in created['rendered_context'] and account(created))
 got=task('get');check('fresh get preserves exact rendering',got['rendered_context']==created['rendered_context'] and account(got))
 zero=task('get',max_context_bytes=0);check('zero budget omits all working text',zero.get('rendered_context')=='' and not zero.get('projection',{}).get('items') and scope+' condition may apply' not in json.dumps(zero))
 check('working text remains absent from canonical memory',sql(f'SELECT content FROM user_memories WHERE id={mid}')==before)
 check('stale expected revision rejected',rebuild(0).get('status')=='conflict')
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:parallel=list(ex.map(lambda _:rebuild(1),range(4)))
 check('parallel revisions have exactly one winner',sum(x.get('status')=='ok' for x in parallel)==1 and sum(x.get('status')=='conflict' for x in parallel)==3)
 got=task('get');check('parallel winner is revision two',got['projection']['revision']=='2')
 cli=subprocess.run(['docker','exec','-u','1000','-e','AIMEE_API_ENDPOINT=unix:/var/lib/aimee/aimee-http.sock',n['server'],'aimee','task','projection','get','--session_id',sid,'--task_id','101','--json'],capture_output=True,text=True,timeout=70)
 check('compiled CLI projection parity',cli.returncode==0 and json.loads(cli.stdout).get('rendered_context')==got['rendered_context'])
 mcp=api('/v1/mcp/call',dict(tool='task_projection',arguments=dict(operation='get',session_id=sid,task_id='101')))
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
 check('MCP projection parity',len(docs)==1 and docs[0].get('rendered_context')==got['rendered_context'])
 preview=task('promotion_preview',expected_revision='2',target_id=str(mid),claim_index=0)
 check('promotion preview identifies exact revision',preview.get('status')=='ok' and preview['promotion']['revision']=='2' and preview['admission']=='review_required')
 check('stale preview rejected',task('promote',expected_revision='2',target_id=str(mid),claim_index=0,preview_digest='stale').get('status')=='conflict')
 admitted=task('promote',expected_revision='2',target_id=str(mid),claim_index=0,preview_digest=preview['preview_digest'])
 check('promotion uses existing review admission',admitted.get('status')=='ok' and admitted.get('admission')=='review_required')
 check('promotion cannot directly overwrite canonical target',sql(f'SELECT content FROM user_memories WHERE id={mid}')==before)
 retried=task('promote',expected_revision='2',target_id=str(mid),claim_index=0,preview_digest=preview['preview_digest'])
 check('identical promotion is idempotent',retried.get('status')=='ok' and retried.get('proposal',{}).get('proposal_id')==admitted.get('proposal',{}).get('proposal_id'))
 proposal=admitted['proposal']
 approved=mem('review_correction',proposal_id=proposal['proposal_id'],payload_digest=proposal['payload_digest'],expected_version=proposal['target_version'],action='approve')
 check('explicit review admits exact proposed hypothesis',approved.get('status')=='ok' and sql(f'SELECT content FROM user_memories WHERE id={mid}').startswith('Task hypothesis: '))
 card=mem('claim-card',id=str(mid),expand_evidence=True)
 check('reviewed hypothesis retains unknown independence', card.get('evidence',{}).get('independence_state')=='unknown')
 retired=mem('delete',id=str(mid),expected_version=card['source_version'])
 if retired.get('proposal'):
  removal=retired['proposal'];retired=mem('review_correction',proposal_id=removal['proposal_id'],payload_digest=removal['payload_digest'],expected_version=removal['target_version'],action='approve')
 check('parent retirement follows governed owner',retired.get('status')=='ok' and sql(f'SELECT lifecycle_state FROM user_memories WHERE id={mid}')!='active')
 invalid=task('get');check('retired parent blocks cached release',invalid.get('reason')=='projection_blocked' and not invalid.get('rendered_context'))
 rebuilt=rebuild(2);check('explicit rebuild recovers valid remaining inputs',rebuilt.get('status')=='ok')
 event=int(sql("INSERT INTO conv_tool_events(session_id,tool_name,tool_result) VALUES('"+sid+"','edit_file','permission denied') RETURNING id"))
 observed=rebuild(3,events=[str(event)])
 check('execution evidence preserves actual failure label',observed.get('status')=='ok' and 'permission denied' in observed.get('rendered_context','') and 'execution_observation' in observed.get('rendered_context',''))
 sql(f"UPDATE conv_tool_events SET tool_result='changed receipt' WHERE id={event}")
 check('changed execution receipt blocks release',task('get').get('reason')=='execution_evidence_unavailable')
 rebuilt=rebuild(4,ttl_seconds=1);check('short lived projection rebuilt',rebuilt.get('status')=='ok')
 time.sleep(1.2)
 check('expired projection cannot release text',task('get').get('status')=='unavailable')
 state=json.loads(sql("SELECT task_projection_state FROM session_state WHERE session_id='"+sid+"'"))
 check('expiry drops derived content and declares replay limit',not state.get('items') and not state.get('dependencies') and state['replay_availability']=='digest_only_projection_not_retained')
 check('expiry preserves execution and canonical records',sql(f'SELECT count(*) FROM conv_tool_events WHERE id={event}')=='1' and sql(f'SELECT count(*) FROM user_memories WHERE id={mid}')=='1')
 discarded=task('discard',expected_revision='5');check('discard is explicit derived-only transition',discarded.get('status')=='ok' and discarded['projection']['state']=='discarded')
 oldid=created['projection']['projection_id'];sql("UPDATE session_state SET active_task_id=102 WHERE session_id='"+sid+"'")
 check('old task access denied after switching',task('get').get('reason')=='active_task_mismatch')
 new=api('/v1/task/projection',dict(operation='rebuild',session_id=sid,task_id='102',expected_revision='0',binding=dict(store='user',project=scope,task=scope,view='briefing'),ttl_seconds=600,items=[]))
 check('task switch creates fresh identity',new.get('status')=='ok' and new['projection']['projection_id']!=oldid and new['projection']['revision']=='1')
 sql("UPDATE server_sessions SET principal='uid:999999' WHERE id='"+sid+"'")
 foreign=api('/v1/task/projection',dict(operation='get',session_id=sid,task_id='102'))
 check('foreign session principal cannot read task state',foreign.get('status')!='ok' and not foreign.get('rendered_context'))
 sql("UPDATE server_sessions SET principal='uid:1000' WHERE id='"+sid+"'")
 subprocess.run(['docker','restart',n['server']],check=True,stdout=subprocess.DEVNULL)
 deadline=time.monotonic()+180
 while time.monotonic()<deadline:
  p=subprocess.run(['docker','inspect','--format','{{.State.Health.Status}}',n['server']],capture_output=True,text=True)
  if p.stdout.strip()=='healthy':break
  time.sleep(2)
 check('candidate healthy after restart',p.stdout.strip()=='healthy')
 recovered=api('/v1/task/projection',dict(operation='get',session_id=sid,task_id='102'))
 check('restart recovers exact task revision with fresh owner',recovered.get('status')=='ok' and recovered['projection']['projection_id']==new['projection']['projection_id'] and account(recovered))
 (out/'summary.json').write_text(json.dumps(dict(complete=True,checks=len(checks),candidate=head,optional_policies_promoted=False),indent=2)+'\n')
 print('MR13_TASK_ACCEPTANCE_COMPLETE',len(checks),flush=True)
finally:
 for session in sessions:
  sql("DELETE FROM conv_tool_events WHERE session_id='"+session+"'; DELETE FROM session_state WHERE session_id='"+session+"'; DELETE FROM server_sessions WHERE id='"+session+"'")
 if ids:sql("BEGIN; SELECT set_config('aimee.memory_explicit_erasure','1',true); DELETE FROM user_memories WHERE id IN ("+','.join(map(str,ids))+") AND key LIKE '"+scope+"%'; COMMIT")
