import json,os,subprocess,time
from pathlib import Path
root=Path('/var/lib/aimee-memory-lab/focused');root.mkdir(exist_ok=True)
env=None
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  args=p.joinpath('cmdline').read_bytes()
  if not (args.startswith(b'go\0test\0') or args.startswith(b'/usr/local/go/bin/go\0test\0')):continue
  e=dict(v.split('=',1) for v in p.joinpath('environ').read_bytes().decode().split('\0') if '=' in v)
  if 'AIMEE_MEMORY_RUNTIME_URL' in e:env=e;break
 except OSError:pass
assert env is not None,'no live suite environment'
results=[]
for name,command in [('recovery-deadline-five-runs',['go','test','-race','-v','-count=5','-run','^TestEvidenceRecoveryDurableRoundPostgres$','./modules/memory']),('utf8-isolated-database',['go','test','-race','-v','-count=1','-run','TestEvaluation','./modules/postgres'])]:
 began=time.monotonic()
 with (root/(name+'.log')).open('w') as log:r=subprocess.run(command,cwd='/opt/aimee/server-go',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
 results.append({'name':name,'exit':r.returncode,'seconds':round(time.monotonic()-began,2)})
 (root/'summary.json').write_text(json.dumps({'checks':results},indent=2))
