import json,pathlib,subprocess,time
p=pathlib.Path('/opt/validation042');deadline=time.monotonic()+1800
while not (p/'evidence/core-images.json').exists():
 if time.monotonic()>deadline:raise RuntimeError('core pull did not complete')
 time.sleep(3)
images=json.loads((p/'evidence/core-images.json').read_text())
for key,name in [('AIMEE_LLM_IMAGE','aimee-llm-e2b'),('AIMEE_LLM_E4B_IMAGE','aimee-llm-e4b')]:
 while True:
  r=subprocess.run(['docker','image','inspect','ghcr.io/rakuensoftware/'+name+':testing'],capture_output=True)
  if r.returncode==0:break
  if time.monotonic()>deadline:raise RuntimeError('model pull did not complete')
  time.sleep(3)
 images[key]=json.loads(r.stdout)[0]['RepoDigests'][0]
for name in ('images.json','browser-images.json'):(p/'evidence'/name).write_text(json.dumps(images,indent=2)+'\n')
raise SystemExit(subprocess.run(['python3','-u',str(p/'run-browser-full.py')],cwd=p).returncode)
