#!/usr/bin/env python3
import json,os,signal,subprocess,sys,time,urllib.request
from pathlib import Path
root=Path('/var/lib/aimee-memory-lab');state=root/'resume-private.json'
if len(sys.argv)>1 and sys.argv[1]=='capture':
 components={}
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   argv=p.joinpath('cmdline').read_bytes().decode().strip('\0').split('\0')
   if '__aimee_supervise_modules' in argv: key='kb-supervisor' if argv[0].endswith('module-kb') else 'server-supervisor'
   elif argv[0]=='/opt/aimee/aimee-kb' and '--http-port=8741' in argv:key='kb'
   elif argv[0]=='/opt/aimee/aimee-server' and any(a.startswith('--socket=') for a in argv):key='server'
   else:continue
   env=dict(v.split('=',1) for v in p.joinpath('environ').read_bytes().decode().split('\0') if '=' in v)
   components[key]={'argv':argv,'env':env}
  except (OSError,IndexError):continue
 assert set(components)=={'kb-supervisor','server-supervisor','kb','server'}
 state.write_text(json.dumps(components));state.chmod(0o600)
 raise SystemExit(0)
components=json.loads(state.read_text());children=[]
def start(key):
 c=components[key];log=(root/(key+'-resumed.log')).open('ab');children.append(subprocess.Popen(c['argv'],env=c['env'],stdout=log,stderr=subprocess.STDOUT))
def cleanup(*args):
 for p in children:
  if p.poll() is None:p.terminate()
 raise SystemExit(0)
signal.signal(signal.SIGTERM,cleanup);signal.signal(signal.SIGINT,cleanup)
start('kb-supervisor');start('kb')
for attempt in range(150):
 if any(p.poll() is not None for p in children):raise RuntimeError('KB component exited')
 try:
  with urllib.request.urlopen('http://127.0.0.1:8741/v1/health',timeout=2) as r:
   if r.status==200:break
 except Exception:time.sleep(1)
else:raise RuntimeError('KB did not become healthy')
start('server-supervisor');start('server')
while True:
 time.sleep(1)
 if any(p.poll() is not None for p in children):raise RuntimeError('Aimee component exited')
