#!/usr/bin/env python3
import hashlib,json,re,subprocess
from pathlib import Path
root=Path('/var/lib/aimee-memory-lab');dest=root/'native-fixes-publishable';dest.mkdir(exist_ok=True)
for name in ['native-fixes','evidence','extended-evidence']:
 source=root/name;output=dest/name;output.mkdir(exist_ok=True)
 for p in source.glob('*.json'):(output/p.name).write_bytes(p.read_bytes())
 if name=='native-fixes':
  target=output/'reboot';target.mkdir(exist_ok=True)
  for p in (source/'reboot').glob('*.json'):(target/p.name).write_bytes(p.read_bytes())
  tests=[]
  for p in source.glob('*.log'):
   if p.name=='build.log':continue
   text=p.read_text(errors='replace');statuses=[{'status':m[1].lower(),'test':m[2],'seconds':float(m[3])} for m in re.finditer(r'^\s*--- (PASS|FAIL|SKIP): (\S+) \(([\d.]+)s\)',text,re.M)]
   tests.append({'stage':p.stem,'tests':statuses,'packages':[l for l in text.splitlines() if re.match(r'^(?:ok\s+|FAIL\s+|\?\s+)github',l)]})
  (output/'go-tests.json').write_text(json.dumps(tests,indent=2))
source_files=['src/modules/kb/c/schema.sql','src/modules/kb/c/schema_grants.sql','server-go/modules/memory/erasure_lineage_test.go','server-go/modules/memory/evidence_recovery_execution.go','server-go/modules/memory/evidence_recovery_execution_test.go','server-go/modules/memory/runtime_role_test.go','server-go/modules/postgres/evaluation.go','server-go/modules/postgres/evaluation_test.go']
manifest={'guest':9212,'source_sha256':{f:hashlib.sha256((Path('/opt/aimee')/f).read_bytes()).hexdigest() for f in source_files},'binary_sha256':{f:hashlib.sha256((Path('/opt/aimee')/f).read_bytes()).hexdigest() for f in ['aimee','aimee-server','aimee-kb','src/build/obj/aimee-module']},'service':subprocess.check_output(['systemctl','show','aimee-memory-validation','-p','ActiveState','-p','ExecMainStatus'],text=True).strip()}
(dest/'manifest.json').write_text(json.dumps(manifest,indent=2))
