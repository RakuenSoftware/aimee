import hashlib,http.client,json,os,socket,sqlite3,sys,threading,time
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
head,phase,procedure_id=sys.argv[1:];prefix='mr17-'+head+'-v3'
root=Path('/var/lib/aimee/'+prefix);root.mkdir(exist_ok=True)
repo=Path('/var/lib/aimee/mr07-activated-fixture/repository');target=repo/'state.c';effective_target=None
checks=json.loads((root/'checks.json').read_text()) if (root/'checks.json').exists() else [];errors=[];ordinal=0
captures=json.loads((root/'captures.json').read_text()) if (root/'captures.json').exists() else []
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
approved='const char *mr07activation_state(void) { return "mr17-verified-'+head+'"; }\n'
checkpoint=json.loads((root/'checkpoint.json').read_text()) if phase=='after' else {}
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  global ordinal,effective_target
  raw=self.rfile.read(int(self.headers['Content-Length']));body=json.loads(raw);ordinal+=1
  response=dict(id=prefix,object='chat.completion',model=prefix,usage=dict(prompt_tokens=1,completion_tokens=1,total_tokens=2),choices=[dict(index=0,finish_reason='stop',message=dict(role='assistant',content='MR17_RETRY_COMPLETE'))]);status=200
  try:
   if ordinal==1:
    digest=hashlib.sha256(raw).hexdigest()
    with sqlite3.connect('file:/var/lib/aimee/audit/worm-live.db?mode=ro',uri=True) as db:row=db.execute("SELECT detail FROM audit_event WHERE actor_role='host' AND actor_principal='uid:1000' AND action='memory.provider.prepared' AND json_extract(detail,'$.binding.payload_sha256')=? ORDER BY seq DESC LIMIT 1",(digest,)).fetchone()
    check(phase+' final provider request has durable receipt',row is not None)
    prepared=json.loads(row[0]);refs=[x for x in prepared['binding']['sources'] if x.get('source_version',{}).get('record_kind')=='learning_procedure' and x.get('stable_id')==procedure_id]
    check(phase+' retained exact current controlled procedure',len(refs)==1)
    captures.append(dict(phase=phase,receipt=prepared['binding_sha256'],source_version=refs[0]['source_version']['version'],payload_sha256=digest));(root/'captures.json').write_text(json.dumps(captures,indent=2)+'\n')
    if phase=='after':
     check('failed speculation absent from clean provider payload',b'MR17_FAILED_SPECULATION' not in raw)
     check('replaced user constraints are absent',b'MR17_OLD_USER_CONSTRAINT' not in raw and b'MR17_CURRENT_USER_CONSTRAINT' in raw)
     check('corrected procedure replaces prior source',b'MR17_NEW_PROCEDURE' in raw and b'MR17_OLD_PROCEDURE' not in raw and captures[-1]['source_version']!=captures[0]['source_version'])
     check('bounded host summary preserves actual write receipt',checkpoint['action'].encode() in raw and b'effect_confirmed' in raw and b'not rolled back' in raw)
   outputs=[m.get('content','') for m in body.get('messages',[]) if m.get('role')=='tool']
   if phase=='before' and ordinal in [1,2]:
    if ordinal==1:name,args='read_file',dict(path=str(target))
    else:
     check('initial read is governed',outputs and 'provider_acknowledged' in str(outputs[-1]));read_id=json.loads(outputs[-1])['action_id'];_,receipt=api('/v1/action/receipt',dict(operation='inspect',session_id=session,action_id=read_id));effective_target=Path(receipt['receipt']['intent']['destination'].removeprefix('file:'))
     name,args='write_file',dict(path=str(effective_target),content=approved)
    response['choices']=[dict(index=0,finish_reason='tool_calls',message=dict(role='assistant',content='MR17_FAILED_SPECULATION: unverified theory',tool_calls=[dict(id='mr17-'+str(ordinal),type='function',function=dict(name=name,arguments=json.dumps(args)))]))]
   elif phase=='before':
    if ordinal==3:
     check('failed attempt really completed a verified write',outputs and 'effect_confirmed' in str(outputs[-1]) and effective_target.read_text()==approved)
     checkpoint.update(action=json.loads(outputs[-1])['action_id'],target=str(effective_target))
    status=400;response={'error':{'message':'controlled MR17 failure after verified write','type':'invalid_request_error'}}
   elif phase=='after' and ordinal==1:
    response['choices']=[dict(index=0,finish_reason='tool_calls',message=dict(role='assistant',content=None,tool_calls=[dict(id='verify-preserved',type='function',function=dict(name='read_file',arguments=json.dumps(dict(path=checkpoint['target']))))]))]
   elif phase=='after':check('retry verifies preserved file without repeating write',outputs and 'provider_acknowledged' in str(outputs[-1]) and 'mr17-verified' in str(outputs[-1]))
  except Exception as exc:errors.append(type(exc).__name__);response['choices']=[dict(index=0,finish_reason='stop',message=dict(role='assistant',content='MR17_FAILED'))]
  wire=json.dumps(response).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(wire)));self.end_headers();self.wfile.write(wire)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
roster=Path('/var/lib/aimee/models.json');old_roster=roster.read_bytes() if roster.exists() else None
try:
 roster.write_text(json.dumps(dict(default_agent=prefix,default_delegate=prefix,fallback_chain=[],models=[dict(name=prefix,model=prefix,provider='mistral',auth_type='none',endpoint=f'http://127.0.0.1:{provider.server_port}/v1',roles=['all'],enabled=True,tools_enabled=True,context_window=16384,max_tokens=4096,max_output=4096,max_parallel=1,max_turns=6)])))
 if phase=='before':
  status,d=api('/v1/sessions/create',dict(client_type=prefix));session=d.get('session_id');check('owned retry session created',status==200 and bool(session));checkpoint['session']=session
 else:session=checkpoint['session'];check('actual file survives server restart',Path(checkpoint['target']).read_text()==approved)
 print('MR17_SESSION '+session,flush=True)
 status,d=api('/v1/sessions/'+session+'/primary',dict(agent=prefix));check(phase+' controlled native provider selected',status==200 and d.get('agent')==prefix)
 options={} if phase=='before' else dict(previous_attempt=checkpoint['attempt'],replace_constraints=True)
 message='mr07activation_state '+('MR17_OLD_USER_CONSTRAINT' if phase=='before' else 'MR17_CURRENT_USER_CONSTRAINT')
 status,d=api('/v1/chat/stream',dict(message=message,aimee_session_id=session,cwd=str(repo),model=prefix,clean_retry=options,evidence_requirements=dict(schema_version=1,task_revision=prefix+':'+phase,query_mode='current_state',obligations=[dict(subject='mr07activation',relation='naming_convention')])))
 (root/(phase+'-response.json')).write_text(json.dumps(dict(status=status,events=d),indent=2)+'\n')
 attempts=[x.get('id') for x in d if isinstance(x,dict) and x.get('event')=='retry_attempt'] if isinstance(d,list) else []
 check(phase+' host emits durable attempt identity',len(attempts)==1 and len(attempts[0])==64)
 if phase=='before':
  check('controlled provider failure is not reported as completion',not errors and ordinal>=3 and any(x.get('status')=='error' for x in d))
  checkpoint['attempt']=attempts[0];(root/'checkpoint.json').write_text(json.dumps(checkpoint)+'\n')
 else:
  check('clean retry completes with a new attempt',not errors and ordinal==2 and attempts[0]!=checkpoint['attempt'] and any(x.get('event')=='done' for x in d))
  check('clean retry never restores or repeats original write',Path(checkpoint['target']).read_text()==approved)
finally:
 if old_roster is None:roster.unlink(missing_ok=True)
 else:roster.write_bytes(old_roster)
 provider.shutdown()
