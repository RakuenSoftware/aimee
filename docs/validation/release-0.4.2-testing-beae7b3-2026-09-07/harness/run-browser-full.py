import json,os,pathlib,subprocess,time
p=pathlib.Path('/opt/validation042');images=json.loads((p/'evidence/browser-images.json').read_text());results=[]
env=dict(os.environ,**images,NODE_PATH=str(p/'browser/node_modules'),CHROMIUM_PATH='/opt/validation042/chromium-app-netns.py',SETUP_E2E_CREDENTIALS=str(p/'private/browser.json'))
def run(name,args):
 start=time.monotonic()
 with (p/'private'/(name+'.log')).open('w') as log:
  rc=subprocess.run(args,cwd=p,env=env,stdout=log,stderr=subprocess.STDOUT).returncode
 results.append({'name':name,'passed':rc==0,'elapsed_seconds':round(time.monotonic()-start,1)})
 (p/'evidence/browser-suite.json').write_text(json.dumps(results,indent=2)+'\n')
 print(name,'PASS' if rc==0 else 'FAIL',flush=True)
 if rc:raise SystemExit(rc)
def state():return json.loads((p/'private/managed-state.json').read_text())
def healthy(container):
 for _ in range(180):
  r=subprocess.run(['docker','inspect','--format','{{.State.Health.Status}}',container],capture_output=True,text=True)
  if r.stdout.strip()=='healthy':return
  time.sleep(2)
 raise RuntimeError('Test container health timeout')
run('managed',['python3','managed.py'])
run('browser-setup',['python3','browser-run.py','setup'])
run('model-lifecycle',['python3','browser-run.py','models'])
healthy(state()['project']+'-aimee-llm-1')
run('navigation',['python3','browser-run.py','navigation'])
run('live-model-browser',['node','live-model-browser.cjs'])
run('providers',['python3','browser-run.py','providers'])
s=state();subprocess.run(['docker','restart',s['application']],check=True,stdout=subprocess.DEVNULL);healthy(s['application'])
subprocess.run(['docker','exec','-d',s['application'],'python3','/tmp/provider-fixture.py','--port','18765'],check=True)
run('providers-after-restart',['python3','browser-run.py','after-restart'])
run('providers-exploratory',['python3','browser-run.py','exploratory'])
run('http-cli-exploratory',['python3','explore.py'])
run('database-contention',['python3','recall-delay.py'])
run('provider-outage',['python3','provider-outage.py'])
run('model-probes',['python3','model-probes.py'])
# Restore the GUI-selected E2B after probing both synthesis variants.
s=state();subprocess.run(['docker','compose','--env-file','/dev/null','-p',s['project'],'-f',s['file'],'-f',s['override'],'--profile','synthesis','up','-d','--no-deps','--no-build','--pull','never','aimee-llm'],cwd=p/'source',env=s['env'],check=True,capture_output=True)
healthy(s['project']+'-aimee-llm-1')
run('container-recreation',['python3','recreate-browser.py'])
run('final-live-model-browser',['node','live-model-browser.cjs'])
