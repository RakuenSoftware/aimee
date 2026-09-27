import hashlib,http.client,json,os,socket,sqlite3,sys,threading,time
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
head,procedure_id=sys.argv[1:3];prefix='mr16-evidence-'+head
root=Path('/var/lib/aimee/'+prefix);root.mkdir(exist_ok=False)
repo=Path('/var/lib/aimee/mr07-activated-fixture/repository');target=repo/'state.c';effective_target=None;approved='const char *mr07activation_state(void) { return \"mr16-fresh-'+head+'\"; }\n'
checks=[];errors=[];captures=[];phase='fresh';ordinal=0;action=None

def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));(root/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise RuntimeError(name)
class Local(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(180);self.sock.connect('/var/lib/aimee/aimee-http.sock')
def api(path,body):
 c=Local('localhost');c.request('POST',path,json.dumps(body),{'Content-Type':'application/json'});r=c.getresponse();raw=r.read();status=r.status;kind=r.getheader('Content-Type','');c.close()
 if 'ndjson' in kind:d=[json.loads(line) for line in raw.splitlines() if line]
 elif 'event-stream' in kind:d=[json.loads(line[5:].strip()) for line in raw.decode().splitlines() if line.startswith('data:') and line[5:].strip()!='[DONE]']
 else:
  try:d=json.loads(raw)
  except ValueError:d=raw.decode()
 return status,d
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  global ordinal,action,effective_target
  raw=self.rfile.read(int(self.headers['Content-Length']));body=json.loads(raw);ordinal+=1
  response=dict(id=prefix,object='chat.completion',model=prefix,usage=dict(prompt_tokens=1,completion_tokens=1,total_tokens=2),choices=[dict(index=0,finish_reason='stop',message=dict(role='assistant',content='MR16_EVIDENCE_OK'))])
  try:
   if ordinal==1:
    digest=hashlib.sha256(raw).hexdigest()
    with sqlite3.connect('file:/var/lib/aimee/audit/worm-live.db?mode=ro',uri=True) as db:
     row=db.execute("SELECT detail FROM audit_event WHERE actor_role='host' AND actor_principal='uid:1000' AND action='memory.provider.prepared' AND json_extract(detail,'$.binding.payload_sha256')=? ORDER BY seq DESC LIMIT 1",(digest,)).fetchone()
    check(phase+' actual provider request has a durable receipt',row is not None)
    prepared=json.loads(row[0]);binding=prepared['binding'];refs=[x for x in binding['sources'] if x.get('source_version',{}).get('record_kind')=='learning_procedure' and x.get('stable_id')==procedure_id]
    check(phase+' exact provider payload retained controlled procedure',len(refs)==1 and b'MR16_PROCEDURE_MARKER' in raw)
    captures.append(dict(phase=phase,receipt=prepared['binding_sha256'],source_version=refs[0]['source_version']['version'],payload_sha256=digest))
   outputs=[m.get('content','') for m in body.get('messages',[]) if m.get('role')=='tool']
   if phase=='fresh' and ordinal<=2:
    if ordinal==1:name,args='read_file',dict(path=str(target))
    else:
     check('memory-bound native read is acknowledged',bool(outputs) and 'provider_acknowledged' in str(outputs[-1]))
     read_id=json.loads(outputs[-1])['action_id'];_,read_receipt=api('/v1/action/receipt',dict(operation='inspect',session_id=session,action_id=read_id));effective_target=Path(read_receipt['receipt']['intent']['destination'].removeprefix('file:'))
     check('receipt names rewritten owned worktree object',effective_target.is_file() and str(effective_target).startswith(str(repo)+'/'))
     name,args='write_file',dict(path=str(effective_target),content=approved)
    response['choices']=[dict(index=0,finish_reason='tool_calls',message=dict(role='assistant',content=None,tool_calls=[dict(id='mr16-'+str(ordinal),type='function',function=dict(name=name,arguments=json.dumps(args)))]))]
   elif phase=='fresh' and ordinal==3:
    check('memory-bound write reports exact verified effect',bool(outputs) and 'effect_confirmed' in str(outputs[-1]));action=json.loads(outputs[-1])['action_id']
   elif phase=='stale' and ordinal==1:
    print('MR16_MUTATE_PROCEDURE',flush=True)
    if sys.stdin.readline().strip()!='continue':raise RuntimeError('controller did not commit mutation')
    response['choices']=[dict(index=0,finish_reason='tool_calls',message=dict(role='assistant',content=None,tool_calls=[dict(id='stale-write',type='function',function=dict(name='write_file',arguments=json.dumps(dict(path=str(effective_target),content='STALE_EFFECT_MUST_NOT_HAPPEN'))))]))]
   elif phase=='stale' and outputs:
    check('stale evidence tool response contains no effect confirmation','effect_confirmed' not in str(outputs[-1]))
  except Exception as exc:errors.append(type(exc).__name__);response['choices']=[dict(index=0,finish_reason='stop',message=dict(role='assistant',content='MR16_EVIDENCE_FAILED'))]
  wire=json.dumps(response).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(wire)));self.end_headers();self.wfile.write(wire)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
roster=Path('/var/lib/aimee/models.json');prior=roster.read_bytes() if roster.exists() else None
try:
 roster.write_text(json.dumps(dict(default_agent=prefix,default_delegate=prefix,fallback_chain=[],models=[dict(name=prefix,model=prefix,provider='openai',auth_type='none',endpoint=f'http://127.0.0.1:{provider.server_port}/v1',roles=['all'],enabled=True,tools_enabled=True,context_window=16384,max_tokens=4096,max_output=4096,max_parallel=1,max_turns=6)])))
 status,d=api('/v1/sessions/create',dict(client_type=prefix));session=d.get('session_id');check('owned evidence action session created',status==200 and bool(session))
 print('MR16_SESSION '+session,flush=True)
 status,d=api('/v1/sessions/'+session+'/primary',dict(agent=prefix));check('controlled provider selected',status==200 and d.get('agent')==prefix)
 for phase in ['fresh','stale']:
  ordinal=0
  status,d=api('/v1/chat/stream',dict(message='mr07activation_state',aimee_session_id=session,cwd=str(repo),model=prefix,evidence_requirements=dict(schema_version=1,task_revision=prefix+':'+phase,query_mode='current_state',obligations=[dict(subject='mr07activation',relation='naming_convention')])) )
  (root/(phase+'-response.json')).write_text(json.dumps(dict(status=status,events=d),indent=2)+'\n')
  if phase=='fresh':
   check('fresh governed model turn completes',status==200 and not errors and ordinal==3 and effective_target is not None and effective_target.read_text()==approved and action)
   status,r=api('/v1/action/receipt',dict(operation='inspect',session_id=session,action_id=action));i=r.get('receipt',{}).get('intent',{});f=r.get('receipt',{}).get('freshness',{})
   check('durable action binds exact memory receipt and guarded evidence',r.get('completion_claim')=='effect_confirmed' and len(i.get('evidence_sha256',''))==64 and len(i.get('context_receipt',''))==64 and f.get('memory_required') is True and len(f.get('memory_guard',''))==64)
   (root/'action-receipt.json').write_text(json.dumps(r,indent=2)+'\n')
  else:check('committed procedure correction prevents stale external effect',not errors and effective_target is not None and effective_target.read_text()==approved and len(captures)==2)
 (root/'captures.json').write_text(json.dumps(captures,indent=2)+'\n')
finally:
 if prior is None:roster.unlink(missing_ok=True)
 else:roster.write_bytes(prior)
 provider.shutdown()
