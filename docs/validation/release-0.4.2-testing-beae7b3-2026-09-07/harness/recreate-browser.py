import json,subprocess,time
from pathlib import Path
p=Path('/opt/validation042');s=json.loads((p/'private/managed-state.json').read_text())
old=json.loads(subprocess.check_output(['docker','inspect',s['application']]))[0]
cmd=['docker','compose','--env-file','/dev/null','-p',s['project'],'-f',s['file'],'-f',s['override'],'up','-d','--no-deps','--no-build','--pull','never','--force-recreate','aimee-server']
subprocess.run(cmd,env=s['env'],cwd=p/'source',check=True,capture_output=True)
for n in range(100):
 new=json.loads(subprocess.check_output(['docker','inspect',s['application']]))[0]
 if new['State'].get('Health',{}).get('Status')=='healthy':break
 time.sleep(2)
else:raise RuntimeError('Recreated application did not become healthy')
assert old['Id']!=new['Id'] and old['Image']==new['Image']
# The browser test reads the permanent account created through the initial wizard.
r=subprocess.run(['python3',str(p/'browser-run.py'),'navigation'],capture_output=True,text=True)
(p/'evidence/recreate-browser.log').write_text(r.stdout+r.stderr)
(p/'evidence/container-recreation.json').write_text(json.dumps(dict(new_container=True,same_published_image=True,healthy=True,permanent_account_login_and_navigation=r.returncode==0),indent=2)+'\n')
print('Recreated published application; permanent-account browser check exit',r.returncode)
raise SystemExit(r.returncode)
