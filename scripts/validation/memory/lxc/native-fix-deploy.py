#!/usr/bin/env python3
import json,os,subprocess,time
from pathlib import Path
root=Path('/opt/aimee');out=Path('/var/lib/aimee-memory-lab/native-fixes');env=json.loads(Path('/root/aimee-native-fix-test-env.json').read_text())
while subprocess.check_output(['systemctl','show','aimee-native-fix-validation','-p','ActiveState','--value'],text=True).strip()=='active':time.sleep(5)
if subprocess.check_output(['systemctl','show','aimee-native-fix-validation','-p','ExecMainStatus','--value'],text=True).strip()!='0':raise RuntimeError('native regression gate failed; not deploying')
subprocess.run(['systemctl','stop','aimee-memory-validation'],check=True)
try:
 with (out/'build.log').open('w') as log:subprocess.run(['make','-C','src','-j4','../aimee','../aimee-server','../aimee-kb','../aimee-delegate-egress','build/obj/aimee-module','build/obj/aimee-module-config'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 for name in ['aimee','aimee-server','aimee-kb']:subprocess.run(['install','-m0755',str(root/name),'/usr/local/bin/'],check=True)
 run=Path(Path('/var/lib/aimee-memory-lab/last-run').read_text().strip())
 for module in (run/'module-bundle/go.modules').read_text().splitlines():
  binary=root/'src/build/obj'/('aimee-module-config' if module=='config' else 'aimee-module')
  subprocess.run(['install','-m0755',str(binary),'/usr/local/libexec/aimee-modules/aimee-module-'+module],check=True)
 schema=(root/'src/modules/kb/c/schema.sql').read_text();begin=schema.index('CREATE OR REPLACE FUNCTION kb_subject_erasure_begin(');end=schema.index('CREATE OR REPLACE FUNCTION kb_subject_erasure_complete(',begin)
 grants=(root/'src/modules/kb/c/schema_grants.sql').read_text();gstart=grants.index('DO $privacy_erasure_grants$');gend=grants.index('$privacy_erasure_grants$;',gstart)+len('$privacy_erasure_grants$;')
 for db in ['aimee_validation','aimee_memory_replay']:
  subprocess.run(['runuser','-u','postgres','--','/usr/lib/postgresql/18/bin/psql','-X','-v','ON_ERROR_STOP=1','-d',db],input='BEGIN;\n'+schema[begin:end]+grants[gstart:gend]+'\nCOMMIT;',text=True,check=True,stdout=subprocess.DEVNULL)
finally:subprocess.run(['systemctl','start','aimee-memory-validation'],check=True)
print('Native fixes built and installed in CT9212; identities and stores retained.',flush=True)
