#!/usr/bin/env python3
"""Owned disposable-container provider observer; never export request bodies."""
import http.client,socket,json,sys,os,time,threading,uuid,subprocess
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
class Local(http.client.HTTPConnection):
 def connect(self):self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(90);self.sock.connect('/var/lib/aimee/aimee-http.sock')
def api(path,body=None):
 c=Local('localhost');c.request('GET' if body is None else 'POST',path,None if body is None else json.dumps(body),{'Content-Type':'application/json'});r=c.getresponse();v=r.read();c.close();return r.status,json.loads(v or b'{}')
if os.environ.get('AIMEE_PRIVACY_PROVIDER_FIXTURE') != '1':
 raise SystemExit('explicit disposable privacy fixture required')
a=json.load(sys.stdin);rows=a['canaries'];requests=[]
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));wire=json.dumps(body,ensure_ascii=False);requests.append(dict(canary_ids=[r['id'] for r in rows if r['value'] in wire],tool_results=sum(m.get('role')=='tool' for m in body.get('messages',[]))))
  message,reason=dict(role='assistant',content='PRIVACY_PROBE_COMPLETE'),'stop'
  if a.get('tool') and len(requests)==1:message=dict(role='assistant',content=None,tool_calls=[dict(id='privacy-call',type='function',function=dict(name='search_memory',arguments=json.dumps(dict(query=a['query'],store='user'))))]);reason='tool_calls'
  raw=json.dumps(dict(id='privacy-probe',object='chat.completion',model=body['model'],choices=[dict(index=0,message=message,finish_reason=reason)],usage=dict(prompt_tokens=11,completion_tokens=2,total_tokens=13))).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
p=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=p.serve_forever,daemon=True).start();roster=Path('/var/lib/aimee/models.json');old=roster.read_bytes() if roster.exists() else None;name='privacy-probe-'+uuid.uuid4().hex[:10];env=dict(os.environ,AIMEE_API_ENDPOINT='unix:/var/lib/aimee/aimee-http.sock')
def config(*args):return json.loads(subprocess.check_output(['aimee','--json','config',*args],env=env,text=True))
mode=None
try:
 mode=config('get','kb_mode')['value'];config('set','kb_mode','none');roster.write_text(json.dumps(dict(default_agent=name,default_delegate=name,fallback_chain=[],models=[dict(name=name,model=name,provider='openai',auth_type='none',endpoint=f'http://127.0.0.1:{p.server_port}/v1',roles=['all'],enabled=True,tools_enabled=True,context_window=32768,max_tokens=4096,max_output=4096,max_parallel=1,max_turns=4)])))
 status,r=api('/v1/runs',dict(model=name,input=a['input'],instructions=a.get('instructions','Privacy fixture instruction'),max_output_tokens=32));run_id=r.get('id');end=time.monotonic()+120
 while run_id and time.monotonic()<end:
  _,r=api('/v1/runs/'+run_id)
  if r.get('status') in ('completed','failed','cancelled'):break
  time.sleep(.2)
 print(json.dumps(dict(http_status=status,run_id=run_id,run_status=r.get('status'),provider_requests=requests,error_kind=(r.get('error') or {}).get('code'))))
finally:
 if old is None:roster.unlink(missing_ok=True)
 else:roster.write_bytes(old)
 if mode is not None:config('set','kb_mode',mode)
 p.shutdown();p.server_close()
