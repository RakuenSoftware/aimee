#!/usr/bin/env python3
import json,os,secrets,subprocess,sys,time
from pathlib import Path
backend=sys.argv[1]
while subprocess.check_output(['systemctl','show','aimee-memory-build','-p','ActiveState','--value'],text=True).strip()=='active':time.sleep(2)
if subprocess.check_output(['systemctl','show','aimee-memory-build','-p','ExecMainStatus','--value'],text=True).strip()!='0':raise RuntimeError('build failed')
root=Path('/opt/aimee');out=Path('/var/lib/aimee-memory-lab/suites-rerun');out.mkdir(parents=True,exist_ok=True)
password=secrets.token_hex(24); runtime_password=secrets.token_hex(24)
def sql(text,database='postgres'):
 subprocess.run(['runuser','-u','postgres','--','/usr/lib/postgresql/18/bin/psql','-X','-v','ON_ERROR_STOP=1','-d',database],input=text,text=True,check=True,stdout=subprocess.DEVNULL)
sql(f"ALTER ROLE ct_test_admin PASSWORD '{password}'; ALTER ROLE aimee_store_runtime PASSWORD '{runtime_password}'; ALTER ROLE ct_test_admin SET jit=off; GRANT USAGE ON SCHEMA public TO aimee_store_runtime;",'aimee_memory_replay')
env=os.environ.copy();env.update(PATH='/usr/local/go/bin:'+env.get('PATH',''),GOPATH='/var/cache/aimee-go',GOCACHE='/var/cache/aimee-go-build',GOMODCACHE='/var/cache/aimee-go/pkg/mod',AIMEE_MEMORY_BACKEND='native')
def dsn(db):return f'postgresql://ct_test_admin:{password}@127.0.0.1:5432/{db}'
env.update(AIMEE_KB_STORE_REPLAY_URL=dsn('aimee_memory_replay'),AIMEE_MEMORY_EVAL_URL=dsn('aimee_memory_eval'),AIMEE_DB_TEST_URL=dsn('aimee_evaluator'),AIMEE_MEMORY_RUNTIME_URL=f'postgresql://aimee_store_runtime:{runtime_password}@127.0.0.1:5432/aimee_memory_replay')
results=[]
def run(name,command,cwd=root,timeout=1800):
 start=time.monotonic()
 with (out/(name+'.log')).open('w') as log:
  try:r=subprocess.run(command,cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout);code=r.returncode
  except subprocess.TimeoutExpired:code=124
 result={'name':name,'exit':code,'elapsed_seconds':round(time.monotonic()-start,2)};results.append(result)
 (out/'summary.json').write_text(json.dumps({'backend':backend,'checks':results},indent=2));print(result,flush=True)
run('race-backend-contract',['go','test','-race','-v','-count=1','./memory','./modules/memory','./modules/memory/backendstore','./modules/memory/cognee','./modules/memory/hillock','./modules/egress'],cwd=root/'server-go')
run('required-owner-replay',['make','-C','src','memory-owner-replay-check'])
if backend=='hillock':
 run('real-hillock',['/opt/hillock-venv/bin/python','scripts/validation/memory/run-hillock-contract.py','--source','/opt/hillock','--artifacts',str(out/'hillock-contract')])
if backend=='cognee':
 run('real-cognee',['python3','scripts/validation/memory/run-cognee-contract.py','--python','/opt/cognee-venv/bin/python','--artifacts',str(out/'cognee-contract')])
run('boundaries',['make','-C','src','memory-c-boundary-check','module-egress-check'])
print('Suites complete:',results,flush=True)

raise SystemExit(0 if all(r["exit"]==0 for r in results) else 1)
