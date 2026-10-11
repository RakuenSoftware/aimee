#!/usr/bin/env python3
import json,os,subprocess,sys,time
from pathlib import Path
backend=sys.argv[1]
for unit in ['aimee-memory-build']+(['aimee-'+backend+'-setup'] if backend!='native' else []):
 while subprocess.check_output(['systemctl','show',unit,'-p','ActiveState','--value'],text=True).strip()=='active':time.sleep(2)
 code=subprocess.check_output(['systemctl','show',unit,'-p','ExecMainStatus','--value'],text=True).strip()
 if code!='0':raise RuntimeError(unit+' failed; inspect private setup log')
env=os.environ.copy();env.update(json.loads(Path('/root/aimee-validation-env.json').read_text()))
env.update(GOPATH='/var/cache/aimee-go',GOCACHE='/var/cache/aimee-go-build',GOMODCACHE='/var/cache/aimee-go/pkg/mod',PATH='/usr/local/go/bin:'+env['PATH'],AIMEE_MEMORY_BACKEND=backend,AIMEE_E2E_SKIP_BUILD='1',AIMEE_E2E_KEEP_RUN_ROOT='1',AIMEE_E2E_PROBE_ONLY='1',AIMEE_E2E_HOLD_SECONDS='86400',AIMEE_E2E_PROBE_SCRIPT='/opt/aimee/scripts/aimee-memory-lxc-probe.py',WAIT_SECONDS='180')
if backend!='native':env.update(AIMEE_MEMORY_BACKEND_URL='http://127.0.0.1:8097',AIMEE_MEMORY_BACKEND_AUTH=('api-key' if backend=='cognee' else 'bearer'),AIMEE_MEMORY_BACKEND_TOKEN=Path('/var/lib/aimee-provider/token').read_text().strip())
os.umask(0o077)
os.execve('/bin/bash',['bash','/opt/aimee/scripts/aimee-memory-lxc-stack.sh'],env)
