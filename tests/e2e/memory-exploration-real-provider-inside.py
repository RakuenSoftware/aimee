"""Native real-model collector smoke test; not release-gate evidence."""
import hashlib,http.client,json,socket,sys,threading,time,uuid
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path('/var/lib/aimee/mr07-real-pilot-'+uuid.uuid4().hex)
root.mkdir(mode=0o700)
repo='/var/lib/aimee/mr07-activated-fixture/repository'
lock=threading.Lock();calls=[];errors=[]
class Local(http.client.HTTPConnection):
 def connect(self):
  self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(600);self.sock.connect('/var/lib/aimee/aimee-http.sock')
def api(path,body):
 c=Local('localhost');c.request('POST',path,json.dumps(body),{'Content-Type':'application/json'})
 r=c.getresponse();raw=r.read();status=r.status;c.close()
 try:d=json.loads(raw)
 except ValueError:d=raw.decode()
 return status,d
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  raw=self.rfile.read(int(self.headers['Content-Length']))
  try:
   body=json.loads(raw)
   with lock:
    ident=str(uuid.uuid4());print('EVAL_RELAY '+json.dumps(dict(id=ident,body=body)),flush=True)
    reply=json.loads(sys.stdin.readline())
    if reply.get('id')!=ident or reply.get('status')!=200:raise RuntimeError('relay response mismatch')
    calls.append(dict(request_sha256=hashlib.sha256(raw).hexdigest(),request_bytes=len(raw),evidence_sha256=reply['evidence_sha256'],elapsed_seconds=reply['elapsed_seconds'],usage=reply['body']['usage']))
    (root/'calls.json').write_text(json.dumps(calls,indent=2)+'\n')
   response=json.dumps(reply['body']).encode();status=200
  except Exception as exc:
   errors.append(type(exc).__name__);response=b'{"error":"collector relay failed"}';status=502
  self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(response)));self.end_headers();self.wfile.write(response)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider)
threading.Thread(target=provider.serve_forever,daemon=True).start()
roster=Path('/var/lib/aimee/models.json');prior=roster.read_bytes() if roster.exists() else None
name='mr07-real-pilot';started=time.monotonic()
try:
 roster.write_text(json.dumps(dict(default_agent=name,default_delegate=name,fallback_chain=[],models=[dict(name=name,model='gpt-6-astra',provider='openai',auth_type='none',endpoint=f'http://127.0.0.1:{provider.server_port}/v1',roles=['all'],enabled=True,tools_enabled=True,context_window=16384,max_tokens=4096,max_output=4096,max_parallel=1,max_turns=8)])))
 status,d=api('/v1/sessions/create',dict(client_type=name));assert status==200 and d.get('session_id');session=d['session_id']
 status,d=api('/v1/sessions/'+session+'/primary',dict(agent=name));assert status==200 and d.get('agent')==name
 body=dict(message='Inspect mr07activation_state in state.c and report the exact string returned by that function. Verify the source rather than relying on supplied naming conventions. Do not edit files.',aimee_session_id=session,cwd=repo,model=name,evidence_requirements=dict(schema_version=1,task_revision='pilot:1',query_mode='current_state',obligations=[dict(subject='mr07activation',relation='naming_convention')]))
 status,result=api('/v1/chat/stream',body)
 (root/'response.json').write_text(json.dumps(dict(status=status,result=result),indent=2)+'\n')
 events=[json.loads(line) for line in result.splitlines() if line] if isinstance(result,str) else []
 final_text=''.join(event.get('content','') for event in events if event.get('event')=='text')
 passed=status==200 and bool(calls) and not errors and 'actual-source-value' in final_text and 'wrong-supplied-value' not in final_text
 (root/'verifier.json').write_text(json.dumps(dict(kind='exact-source-answer',passed=passed,final_text=final_text),indent=2)+'\n')
 summary=dict(kind='real_model_collector_smoke',release_evidence=False,passed=passed,status=status,provider_calls=len(calls),latency_seconds=time.monotonic()-started,evidence_directory=str(root),session=session,errors=errors)
 (root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
 print(('PASS ' if passed else 'FAIL ')+json.dumps(summary),flush=True)
 if not passed:raise SystemExit(1)
finally:
 if prior is None:roster.unlink(missing_ok=True)
 else:roster.write_bytes(prior)
 provider.shutdown()
