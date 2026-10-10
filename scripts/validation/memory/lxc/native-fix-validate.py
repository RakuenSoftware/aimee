#!/usr/bin/env python3
import json,os,secrets,subprocess,time
from pathlib import Path
root=Path('/opt/aimee');out=Path('/var/lib/aimee-memory-lab/native-fixes');out.mkdir(exist_ok=True)
env=os.environ.copy();env.update(PATH='/usr/local/go/bin:'+env.get('PATH',''),GOPATH='/var/cache/aimee-go',GOCACHE='/var/cache/aimee-go-build',GOMODCACHE='/var/cache/aimee-go/pkg/mod',AIMEE_MEMORY_BACKEND='native')
password=secrets.token_hex(24);runtime_password=secrets.token_hex(24)
def sql(query,db):
 subprocess.run(['runuser','-u','postgres','--','/usr/lib/postgresql/18/bin/psql','-X','-v','ON_ERROR_STOP=1','-d',db],input=query,text=True,check=True,stdout=subprocess.DEVNULL)
sql(f"ALTER ROLE ct_test_admin PASSWORD '{password}'; ALTER ROLE ct_test_admin RESET jit; ALTER ROLE aimee_store_runtime PASSWORD '{runtime_password}'; GRANT USAGE ON SCHEMA public TO aimee_store_runtime;",'aimee_memory_replay')
def dsn(db):return f'postgresql://ct_test_admin:{password}@127.0.0.1:5432/{db}'
env.update(AIMEE_KB_STORE_REPLAY_URL=dsn('aimee_memory_replay'),AIMEE_MEMORY_EVAL_URL=dsn('aimee_memory_eval'),AIMEE_DB_TEST_URL=dsn('aimee_evaluator'),AIMEE_MEMORY_RUNTIME_URL=f'postgresql://aimee_store_runtime:{runtime_password}@127.0.0.1:5432/aimee_memory_replay',AIMEE_DB_TEST_REQUIRED='1')
private=Path('/root/aimee-native-fix-test-env.json');private.write_text(json.dumps(env));private.chmod(0o600)
results=[]
def run(name,command,cwd=root,timeout=1800):
 start=time.monotonic()
 with (out/(name+'.log')).open('w') as log:
  try:code=subprocess.run(command,cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout).returncode
  except subprocess.TimeoutExpired:code=124
 results.append({'name':name,'exit':code,'seconds':round(time.monotonic()-start,2)});(out/'summary.json').write_text(json.dumps({'checks':results},indent=2));print(results[-1],flush=True)
 return code
if run('targeted-regressions',['go','test','-race','-v','-count=3','-run','TestEvidenceRecoveryAdmission|TestEvidenceRecoveryDurableRoundPostgres|TestSubjectErasureTransitiveCopiesPostgres|TestEvaluation','./modules/memory','./modules/postgres'],root/'server-go')!=0:raise SystemExit(1)
run('required-owner-replay',['make','-C','src','memory-owner-replay-check'])
run('boundaries',['make','-C','src','memory-c-boundary-check','module-egress-check'])
raise SystemExit(0 if all(x['exit']==0 for x in results) else 1)
