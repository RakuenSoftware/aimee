#!/usr/bin/env python3
"""Synthetic provider inside the explicitly owned MR-07 activation fixture."""
import json,os,socket,http.client,time,uuid,threading,subprocess,sqlite3,hashlib
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path('/var/lib/aimee/mr07-activated-fixture');root.mkdir(exist_ok=True)
statefile=root/'phase.json';signalfile=root/'continue.json';checks=[];phase='scope';ordinal=0;errors=[]
key='mr07activation';repo=root/'repository';repo.mkdir(exist_ok=True)
source='const char *mr07activation_state(void) { return "actual-source-value"; }\n'
(repo/'state.c').write_text(source)
for cmd in [['git','init','-b','main',str(repo)],['git','-C',str(repo),'add','.'],['git','-C',str(repo),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-m','Frozen activation fixture']]:subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
class Local(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(120);self.sock.connect('/var/lib/aimee/aimee-http.sock')
def api(path,body=None):
 c=Local('localhost');c.request('POST' if body is not None else 'GET',path,None if body is None else json.dumps(body),{'Content-Type':'application/json'})
 r=c.getresponse();raw=r.read();kind=r.getheader('Content-Type','');status=r.status;c.close()
 if 'ndjson' in kind:return status,[json.loads(line) for line in raw.splitlines() if line]
 if 'event-stream' in kind:return status,[json.loads(line[5:].strip()) for line in raw.decode().splitlines() if line.startswith('data:') and line[5:].strip()!='[DONE]']
 return status,json.loads(raw)
def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));(root/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')
 if not ok:raise RuntimeError(name)
def wait_for(label,extra=None):
 statefile.write_text(json.dumps(dict(phase=label,session=session,key=key,repository=str(repo),source=source,**(extra or {}))))
 deadline=time.monotonic()+240
 while time.monotonic()<deadline:
  if signalfile.exists():
   d=json.loads(signalfile.read_text())
   if d.get('phase')==label:signalfile.unlink();return d
  time.sleep(.2)
 raise RuntimeError('controller did not finish '+label)
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  global ordinal
  body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));ordinal+=1
  if phase in ('coverage','activated') and ordinal==1:
   if 'wrong-supplied-value' not in json.dumps(body):errors.append('typed requirement context absent from actual provider input')
  response=dict(id='activation-fixture',object='chat.completion',model='activation-fixture',usage=dict(prompt_tokens=1,completion_tokens=1,total_tokens=2),choices=[dict(index=0,finish_reason='stop',message=dict(role='assistant',content='ACTIVATION_FIXTURE_OK'))])
  if phase=='activated' and ordinal<=4:
   outputs=[m.get('content','') for m in body.get('messages',[]) if m.get('role')=='tool']
   try:
    if ordinal==1:name,args='grep',dict(path='.',pattern=key,max_results=1)
    elif ordinal==2:
     check('activated literal-zero discovery cap returns a structured restriction',outputs and ('adaptive' in str(outputs[-1]) or 'exploration' in str(outputs[-1])) and ('budget' in str(outputs[-1]) or 'limit' in str(outputs[-1])))
     name,args='find_symbol',dict(identifier=key+'_missing')
    elif ordinal==3:
     gap=json.loads(outputs[-1].split('Exploration recovery: ',1)[1]);name,args='context_contract_expand',dict(reason='indexed evidence did not resolve the wrong supplied claim',gap_ref=gap['gap_ref'],outcome_id=gap['outcome_id'])
    else:
     check('activated contract expands from the owned indexed miss',json.loads(outputs[-1]).get('status')=='ok');name,args='grep',dict(path='.',pattern=key,max_results=1)
    response['choices']=[dict(index=0,finish_reason='tool_calls',message=dict(role='assistant',content=None,tool_calls=[dict(id='activation-'+str(ordinal),type='function',function=dict(name=name,arguments=json.dumps(args)))]))]
   except Exception as exc:errors.append(type(exc).__name__);response['choices'][0]['message']['content']='ACTIVATION_FIXTURE_FAILED'
  if phase=='activated' and ordinal==5:
   outputs=[m.get('content','') for m in body.get('messages',[]) if m.get('role')=='tool']
   try:check('bounded fallback reads actual contradictory source',bool(outputs) and 'actual-source-value' in str(outputs[-1]))
   except Exception as exc:errors.append(type(exc).__name__);response['choices'][0]['message']['content']='ACTIVATION_FIXTURE_FAILED'
  raw=json.dumps(response).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
roster=Path('/var/lib/aimee/models.json');prior=roster.read_bytes() if roster.exists() else None
persona=roster.parent/'personas'/'mr07-activation.md';persona.parent.mkdir(exist_ok=True)
try:
 roster.write_text(json.dumps(dict(default_agent=key,default_delegate=key,fallback_chain=[],models=[dict(name=key,model='activation-fixture',provider='openai',auth_type='none',endpoint=f'http://127.0.0.1:{provider.server_port}/v1',roles=['all'],enabled=True,tools_enabled=True,context_window=16384,max_tokens=4096,max_output=4096,max_parallel=1,max_turns=8)])))
 persona.write_text('## Persona\nInspect the isolated activation fixture.\n## Principles\nVerify supplied claims with indexed tools and source reads.\n')
 for setting in [('memory_recall_enabled',1),('ingress_preinject_enabled',True),('code_context_mode','on')]:
  status,result=api('/v1/config/set',dict(key=setting[0],value=setting[1]));check('fixture config '+setting[0],status==200 and result.get('status')=='ok')
 status,d=api('/v1/sessions/create',dict(client_type='mr07-activation-fixture'));session=d.get('session_id');check('owned activation session',status==200 and bool(session))
 status,d=api('/v1/sessions/'+session+'/primary',dict(agent=key));check('activation provider pinned',status==200 and d.get('agent')==key)
 api('/v1/sessions/'+session+'/persona',dict(name='mr07-activation'))
 def turn(message,requirements=False):
  global ordinal
  ordinal=0;body=dict(message=message,aimee_session_id=session,cwd=str(repo),model=key)
  if requirements:body['evidence_requirements']=dict(schema_version=1,task_revision='activation:1',query_mode='current_state',obligations=[dict(subject=key,relation='naming_convention')])
  status,d=api('/v1/chat/stream',body);(root/(phase+'-response.json')).write_text(json.dumps(dict(status=status,events=d)));check('primary '+phase+' completes',status==200 and 'ACTIVATION_FIXTURE_OK' in json.dumps(d) and not errors)
 turn('Locate '+key+' source for activation preflight.')
 wait_for('scope')
 phase='coverage';turn('mr07activation_state',True)
 wait_for('coverage')
 phase='activated';turn('mr07activation_state',True)
 check('activated native recovery reaches five provider turns',ordinal==5)
 wait_for('activated')
 phase='revoked';turn('mr07activation_state',True)
 wait_for('revoked')
finally:
 if prior is None:roster.unlink(missing_ok=True)
 else:roster.write_bytes(prior)
 persona.unlink(missing_ok=True);provider.shutdown()
