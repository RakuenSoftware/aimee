import datetime,hashlib,http.client,json,os,socket,sqlite3,sys,threading,time,subprocess
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
head,procedure_id=sys.argv[1:3];assert os.getuid()==1000
root=Path('/var/lib/aimee/mr15-live-'+head);root.mkdir(exist_ok=False)
checks=[];captures=[];errors=[];prefix='mr15-'+head

def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));(root/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise RuntimeError(name)
class Local(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(120);self.sock.connect('/var/lib/aimee/aimee-http.sock')
def api(path,body):
 c=Local('localhost');c.request('POST',path,json.dumps(body),{'Content-Type':'application/json'});r=c.getresponse();raw=r.read();status=r.status;c.close()
 try:d=json.loads(raw)
 except ValueError:d=raw.decode()
 return status,d
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  raw=self.rfile.read(int(self.headers['Content-Length']));digest=hashlib.sha256(raw).hexdigest()
  try:
   with sqlite3.connect('file:/var/lib/aimee/audit/worm-live.db?mode=ro',uri=True) as db:
    row=db.execute("SELECT subject,detail FROM audit_event WHERE actor_role='host' AND actor_principal='uid:1000' AND action='memory.provider.prepared' AND json_extract(detail,'$.binding.payload_sha256')=? ORDER BY seq DESC LIMIT 1",(digest,)).fetchone()
   check('actual provider bytes have a durable prepared receipt',row is not None)
   attempt,prepared=row[0],json.loads(row[1]);binding=prepared['binding']
   refs=[ref for ref in binding['sources'] if ref.get('source_version',{}).get('record_kind')=='learning_procedure' and ref.get('stable_id')==procedure_id]
   check('reviewed procedure retained in the exact final request',len(refs)==1 and b'MR15_PROCEDURE_MARKER' in raw)
   v=refs[0]['source_version']['version'];captures.append(dict(attempt=attempt,request=binding['request_id'],receipt=prepared['binding_sha256'],procedure=dict(owner_id=v['owner_id'],procedure_id=v['record_id'],revision=v['record_revision']),payload_sha256=digest,payload_bytes=len(raw)))
  except Exception as exc:errors.append(type(exc).__name__)
  body=json.dumps(dict(id='mr15',object='chat.completion',model='mr15',usage=dict(prompt_tokens=1,completion_tokens=1,total_tokens=2),choices=[dict(index=0,finish_reason='stop',message=dict(role='assistant',content='MR15_OK' if not errors else 'MR15_FAILED'))])).encode()
  self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
roster=Path('/var/lib/aimee/models.json');prior=roster.read_bytes() if roster.exists() else None;name=prefix

def event(c,event_id,trial,state,terminal=False):
 return dict(schema_version=1,event_id=prefix+'-'+event_id,trial_id=trial,task_id=prefix+'-task',attempt_id=c['attempt'],procedure=c['procedure'],receipt_ref=c['receipt'],state=state,authority='user_feedback',environment='ct109-fixture',model='controlled-fake-provider',task_class='protocol-acceptance',action_refs=['fixture:inspect-receipt'],verifier='explicit_user_evaluation',verification_ref='fixture:protocol-check',observed_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),terminal_task=terminal,reported_latency_ms='1')
def submit(c,e,**extra):return api('/v1/learning/application',dict(request_id=c['request'],event=e,**extra))[1]
def cli(args):
 p=subprocess.run(['aimee',*args,'--json'],env=dict(os.environ,AIMEE_API_ENDPOINT='unix:/var/lib/aimee/aimee-http.sock'),capture_output=True,text=True,timeout=60)
 return p.returncode,json.loads(p.stdout) if p.stdout.strip().startswith('{') else {}
try:
 roster.write_text(json.dumps(dict(default_agent=name,default_delegate=name,fallback_chain=[],models=[dict(name=name,model='mr15',provider='openai',auth_type='none',endpoint=f'http://127.0.0.1:{provider.server_port}/v1',roles=['all'],enabled=True,tools_enabled=True,context_window=16384,max_tokens=4096,max_output=4096,max_parallel=1,max_turns=1)])))
 status,d=api('/v1/sessions/create',dict(client_type=name));check('fixture session created',status==200 and bool(d.get('session_id')));session=d['session_id']
 status,d=api('/v1/sessions/'+session+'/primary',dict(agent=name));check('controlled provider selected',status==200 and d.get('agent')==name)
 for i in range(3):
  if i==2:
   print('MR15_CHANGE_VERSION',flush=True);assert sys.stdin.readline().strip()=='continue'
  before=len(captures)
  status,d=api('/v1/chat/stream',dict(message='mr07activation_state',aimee_session_id=session,cwd='/var/lib/aimee/mr07-activated-fixture/repository',model=name,evidence_requirements=dict(schema_version=1,task_revision=prefix+':1',query_mode='current_state',obligations=[dict(subject='mr07activation',relation='naming_convention')])))
  check('native turn completes with a bound final request',status==200 and 'MR15_OK' in str(d) and not errors and len(captures)==before+1)
  c=captures[-1];status,receipt=api('/v1/memory/receipt',dict(request_id=c['request']))
  matches=[x for r in receipt.get('receipts',[]) for x in r.get('procedure_exposures',[]) if x.get('receipt_ref')==c['receipt'] and x.get('procedure')==c['procedure']]
  check('principal receipt reader attests acknowledged procedure delivery',status==200 and len(matches)==1 and matches[0]['state']=='delivered')
  if i==0:
   e=event(c,'success','trial-1','verified_success');d=submit(c,e,exposure={'producer':'forged'},scope_kind='project',scope_id='forged')
   if d.get('status')!='ok':(root/'admission-diagnostic.json').write_text(json.dumps(d)+'\n')
   check('explicit feedback produces governed experience',d.get('status')=='ok' and d['experience'][0]['verified_success']==1)
   first=d['event_ref'];again=submit(c,e);check('duplicate event reuses immutable identity',again.get('duplicate') is True and again['event_ref']==first and again['experience'][0]['trials']==1)
   changed=dict(e,state='verified_failure');check('conflicting replacement rejected',submit(c,changed).get('status')=='error')
   model=dict(e,event_id=prefix+'-model',authority='model');check('model self-reward rejected',submit(c,model).get('status')=='error')
   omitted=dict(e,event_id=prefix+'-omitted',procedure=dict(c['procedure'],procedure_id='999999999'));check('omitted procedure cannot gain success',submit(c,omitted).get('status')=='error')
   unused=dict(e,event_id=prefix+'-unused',action_refs=[]);check('unused procedure cannot gain success',submit(c,unused).get('status')=='error')
   conflict=submit(c,dict(e,event_id=prefix+'-failure',state='verified_failure'));check('distinct conflicting evidence is preserved',conflict.get('status')=='ok' and conflict['experience'][0]['conflicting']==1 and conflict['experience'][0]['verified_success']==0)
  elif i==1:
   e=event(c,'retry-success','trial-2','verified_success',True);code,d=cli(['learning','application',c['request'],'--event-json',json.dumps(e)])
   check('compiled CLI records a separate repair attempt',code==0 and d.get('event_ref') and d['experience'][0]['trials']==2 and d['experience'][0]['terminal_tasks']==1 and d['experience'][0]['verified_success']==1)
   old_version=c['procedure']['revision']
  else:
   check('procedure edit creates a new canonical version',c['procedure']['revision']!=old_version)
   d=submit(c,event(c,'new-version','trial-3','outcome_unknown'));check('new version starts an unknown cohort',d.get('status')=='ok' and d['experience'][0]['trials']==1 and d['experience'][0]['outcome_unknown']==1 and d['experience'][0]['verified_success']==0)
 stages={s:'not_applicable:fixture' for s in ['indexing_embedding','retrieval_reranking','context_transformation','generation','tools','retries','verification']};stages['generation']='observed';stages['verification']='observed';stages['indexing_embedding']='unknown'
 base=dict(id='call',attempt_id=captures[0]['attempt'],stage='generation',kind='actual',nanodollars='9007199254740993',provider='synthetic-ledger',model='fixture',pricing_snapshot='fixture:invoice',cache_assumptions='fixture:no-cache',allocation='marginal',allocation_rule='whole synthetic charge',evidence_ref='fixture:invoice',latency_ms='1')
 unpriced=dict(base,id='verification',stage='verification',kind='unpriced');unpriced.pop('nanodollars')
 cost=dict(schema_version=1,task_id=prefix+'-task',stages=stages,entries=[base,unpriced,base])
 status,d=api('/v1/learning/task_cost',dict(cost=cost));r=d.get('report',{})
 check('native cost owner preserves exact integers duplicates and unknown coverage',status==200 and r.get('actual_nanodollars')=='9007199254740993' and r.get('unpriced_entry_ids')==['verification'] and r.get('coverage')=='incomplete' and len(r.get('entries',[]))==2)
 code,d=cli(['learning','task-cost','--cost-json',json.dumps(cost)]);check('compiled CLI cost parity',code==0 and d.get('report')==r)
 corpus=[dict(task_id=cost['task_id'],task_sha256='1'*64,verifier_sha256='2'*64)];corpus_digest=hashlib.sha256(json.dumps(corpus,separators=(',',':')).encode()).hexdigest();arm=dict(cost=cost,completion='completed',quality_ppm=1000000,quality_evidence_ref='fixture:quality')
 pair=dict(schema_version=1,corpus_sha256=corpus_digest,corpus=corpus,baseline=[arm],candidate=[arm]);status,d=api('/v1/learning/task_cost',dict(cost=pair));r=d.get('report',{})
 check('paired cost remains unqualified with unknown pricing',status==200 and r.get('pairs')==1 and r.get('incomplete_cost_pairs')==1 and str(r.get('release_gate')).startswith('unqualified'))
 pair['candidate']=[];check('missing paired failures cannot be silently excluded',api('/v1/learning/task_cost',dict(cost=pair))[1].get('status')=='error')
 (root/'captures.json').write_text(json.dumps(captures,indent=2)+'\n');print('MR15_COMPLETE '+str(root),flush=True)
finally:
 if prior is None:roster.unlink(missing_ok=True)
 else:roster.write_bytes(prior)
 provider.shutdown()
