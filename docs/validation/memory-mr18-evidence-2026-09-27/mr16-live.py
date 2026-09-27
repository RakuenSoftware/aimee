import json,subprocess,sys,re,time
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
n=json.loads(Path('/opt/pr2990-evidence/mr07-managed-be46915db/names.json').read_text())
out=Path('/opt/pr2990-evidence/mr16-actions-'+head);out.mkdir(exist_ok=False)
subprocess.run(['docker','cp','/opt/pr2990-mr16-live-inside.py',n['server']+':/tmp/mr16-live.py'],check=True,stdout=subprocess.DEVNULL)
try:
 for phase in ['before','after']:
  if phase=='after':
   subprocess.run(['docker','restart',n['server']],check=True,stdout=subprocess.DEVNULL)
   deadline=time.monotonic()+180
   while time.monotonic()<deadline:
    status=subprocess.check_output(['docker','inspect','--format','{{.State.Health.Status}}',n['server']],text=True).strip()
    if status=='healthy':break
    time.sleep(2)
   else:raise RuntimeError('candidate did not become healthy after restart')
   print('candidate server actually restarted and healthy',flush=True)
  with (out/(phase+'.log')).open('w') as log:
   process=subprocess.Popen(['docker','exec','-u','1000',n['server'],'python3','/tmp/mr16-live.py',head,phase],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
   for line in process.stdout:log.write(line);log.flush();print(line,end='',flush=True)
   if process.wait():raise RuntimeError('MR16 '+phase+' acceptance failed')
finally:
 for file in ['checks.json','checkpoint.json','timing.json','last-response.json']:
  subprocess.run(['docker','cp',n['server']+':/var/lib/aimee/mr16-'+head+'/'+file,str(out/file)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
