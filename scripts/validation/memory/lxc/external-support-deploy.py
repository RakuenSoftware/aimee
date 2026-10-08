#!/usr/bin/env python3
"""Upgrade only the already-provisioned disposable Cognee/Hillock test guests."""
import os,subprocess,sys
from pathlib import Path
backend=sys.argv[1]
if backend not in ('cognee','hillock'):raise SystemExit('external test backend required')
root=Path('/opt/aimee');out=Path('/var/lib/aimee-memory-lab/external-support');out.mkdir(exist_ok=True)
env=os.environ.copy();env.update(PATH='/usr/local/go/bin:'+env.get('PATH',''),GOPATH='/var/cache/aimee-go',GOCACHE='/var/cache/aimee-go-build',GOMODCACHE='/var/cache/aimee-go/pkg/mod')
with (out/'regression.log').open('w') as log:
 subprocess.run(['go','test','-race','-short','-count=1','./memory','./modules/memory','./modules/memory/backendstore','./modules/memory/cognee','./modules/memory/hillock','./modules/egress'],cwd=root/'server-go',env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
with (out/'transfer-build.log').open('w') as log:
 subprocess.run(['go','build','-o','/opt/aimee/src/build/obj/aimee-memory-transfer','./cmd/aimee-memory-transfer'],cwd=root/'server-go',env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
with (out/'build.log').open('w') as log:
 subprocess.run(['make','-C','src','-j4','../aimee','../aimee-server','../aimee-kb','../aimee-delegate-egress','build/obj/aimee-module','build/obj/aimee-module-config'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
subprocess.run(['systemctl','stop','aimee-memory-validation'],check=True)
try:
 for name in ['aimee','aimee-server','aimee-kb']:subprocess.run(['install','-m0755',str(root/name),'/usr/local/bin/'],check=True)
 run=Path(Path('/var/lib/aimee-memory-lab/last-run').read_text().strip())
 for module in (run/'module-bundle/go.modules').read_text().splitlines():
  binary=root/'src/build/obj'/('aimee-module-config' if module=='config' else 'aimee-module')
  subprocess.run(['install','-m0755',str(binary),'/usr/local/libexec/aimee-modules/aimee-module-'+module],check=True)
 subprocess.run(['install','-m0755',str(root/'scripts/validation/memory/lxc/probe.py'),str(root/'scripts/aimee-memory-lxc-probe.py')],check=True)
finally:subprocess.run(['systemctl','start','aimee-memory-validation'],check=True)
print('External support installed in disposable '+backend+' guest.',flush=True)
