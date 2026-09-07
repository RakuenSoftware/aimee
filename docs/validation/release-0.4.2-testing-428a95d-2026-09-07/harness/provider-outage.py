import json,subprocess,time
from pathlib import Path
p=Path('/opt/validation042');s=json.loads((p/'private/managed-state.json').read_text())
code='''from pathlib import Path
import os,signal
matches=[]
for p in Path('/proc').glob('[0-9]*/cmdline'):
 try:
  args=p.read_bytes().split(bytes([0]));exe=args[0].decode()
  if Path(exe).name=='aimee-module-providers' and len(args)>1 and args[1].endswith(b'/server-module-bus.sock'):matches.append(int(p.parent.name))
 except (OSError,UnicodeError):pass
assert len(matches)==1, 'Expected one providers process, found '+str(len(matches))
os.kill(matches[0],signal.SIGSTOP)
print(matches[0])
'''
r=subprocess.run(['docker','exec',s['application'],'python3','-c',code],capture_output=True,text=True,check=True);pid=int(r.stdout)
try:
 r=subprocess.run(['python3',str(p/'browser-run.py'),'module-down'],capture_output=True,text=True,timeout=180)
 (p/'evidence/provider-outage.log').write_text(r.stdout+r.stderr);print(r.stdout);print('provider outage exit',r.returncode)
finally:
 subprocess.run(['docker','exec',s['application'],'python3','-c',f'import os,signal;os.kill({pid},signal.SIGKILL)'],check=True,capture_output=True)
# Give packaged supervision a bounded opportunity to restore the detached module.
code='''from pathlib import Path
import json,socket,http.client
class C(http.client.HTTPConnection):
 def connect(self):self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(10);self.sock.connect('/var/lib/aimee/aimee-http.sock')
c=C('localhost',timeout=10);c.request('GET','/v1/provider/connections');r=c.getresponse();print(json.dumps([r.status,json.loads(r.read())]))
'''
for n in range(30):
 try:
  probe=subprocess.run(['docker','exec',s['application'],'python3','-c',code],capture_output=True,text=True,timeout=15)
  response=json.loads(probe.stdout)
  if response[0]==200 and isinstance(response[1].get('providers'),list):break
 except (ValueError,subprocess.TimeoutExpired):pass
 time.sleep(2)
else:raise RuntimeError('Providers did not recover through packaged supervision')
(p/'evidence/provider-recovery.json').write_text(json.dumps(dict(passed=True,check='Providers recover through packaged supervision after an unresponsive module is killed'))+'\n')
print('PASS providers recover through packaged supervision')
raise SystemExit(r.returncode)
