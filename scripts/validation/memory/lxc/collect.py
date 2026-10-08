#!/usr/bin/env python3
import hashlib,json,re,subprocess,sys
from pathlib import Path
backend=sys.argv[1];root=Path('/var/lib/aimee-memory-lab');dest=root/'publishable';dest.mkdir(exist_ok=True)
for name in ['evidence','extended-evidence','qualification-service-resume','qualification-guest-reboot','qualification-verified-tls','suites','suites-rerun','focused']:
 source=root/name
 if not source.exists():continue
 output=dest/name;output.mkdir(exist_ok=True)
 for p in source.glob('*.json'):
  if p.name=='summary.json' or p.name=='hillock-language-probes.json':(output/p.name).write_bytes(p.read_bytes())
  elif p.name.startswith('api-'):
   data=json.loads(p.read_text());resp=data.get('response',{})
   if resp.get('status') in ('error','failed') or data.get('route')=='kb/erase-subject':(output/p.name).write_bytes(p.read_bytes())
 tests=[]
 for p in source.glob('*.log'):
  text=p.read_text(errors='replace')
  statuses=[{'status':m.group(1).lower(),'test':m.group(2),'seconds':float(m.group(3))} for m in re.finditer(r'^\s*--- (PASS|FAIL|SKIP): (\S+) \(([\d.]+)s\)',text,re.M)]
  tests.append({'stage':p.stem,'tests':statuses,'packages':[line for line in text.splitlines() if re.match(r'^(?:ok\s+|FAIL\s+|\?\s+)github',line)]})
 if tests:(output/'go-tests.json').write_text(json.dumps(tests,indent=2))
 for subdir in ['hillock-contract','cognee-contract']:
  p=source/subdir/'summary.json'
  if p.exists():(output/(subdir+'-summary.json')).write_bytes(p.read_bytes())
run=Path((root/'last-run').read_text().strip())
versions={'backend':backend,'postgres':subprocess.check_output(['/usr/lib/postgresql/18/bin/postgres','--version'],text=True).strip(),'go':subprocess.check_output(['/usr/local/go/bin/go','version'],text=True).strip(),'os':Path('/etc/os-release').read_text(),'aimee_source_base':'0b298d23c plus working tree and recorded patches','source_sha256':{},'binaries_sha256':{},'run_root':str(run),'service':subprocess.check_output(['systemctl','show','aimee-memory-validation','-p','ActiveState','-p','ExecMainStatus'],text=True).strip()}
for name in ['server-go/modules/memory/external_store.go','server-go/modules/memory/external_store_test.go','server-go/modules/postgres/evaluation.go','server-go/modules/postgres/evaluation_test.go','integrations/hillock/service.py']:
 p=Path('/opt/aimee')/name;versions['source_sha256'][name]=hashlib.sha256(p.read_bytes()).hexdigest()
for name in ['aimee','aimee-server','aimee-kb','src/build/obj/aimee-module']:
 p=Path('/opt/aimee')/name;versions['binaries_sha256'][name]=hashlib.sha256(p.read_bytes()).hexdigest()
if backend=='hillock':versions['hillock_revision']=Path('/opt/hillock/AIMEE_UPSTREAM_REVISION').read_text().strip()
if backend=='cognee':versions['cognee_version']=subprocess.check_output(['/opt/cognee-venv/bin/python','-c','import importlib.metadata;print(importlib.metadata.version("cognee"))'],text=True).strip()
(dest/'environment.json').write_text(json.dumps(versions,indent=2))
