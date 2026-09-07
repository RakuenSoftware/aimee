import json,pathlib,subprocess
p=pathlib.Path('/opt/validation042');s=json.loads((p/'private/managed-state.json').read_text());images=json.loads((p/'evidence/browser-images.json').read_text());checks=[]
for service,key in [('aimee-server','AIMEE_APPLICATION_IMAGE'),('aimee-store-db','AIMEE_POSTGRES_IMAGE'),('aimee-embedder','AIMEE_EMBEDDER_IMAGE'),('aimee-llm','AIMEE_LLM_IMAGE')]:
 name=s['project']+'-'+service+'-1';container=json.loads(subprocess.check_output(['docker','inspect',name]))[0];expected=json.loads(subprocess.check_output(['docker','image','inspect',images[key]]))[0]
 checks.append(dict(name=service+' healthy and matches published image',passed=container['State']['Health']['Status']=='healthy' and container['Image']==expected['Id']))
(p/'evidence/final-runtime.json').write_text(json.dumps(checks,indent=2)+'\n');print(json.dumps(checks))
assert all(r['passed'] for r in checks)
