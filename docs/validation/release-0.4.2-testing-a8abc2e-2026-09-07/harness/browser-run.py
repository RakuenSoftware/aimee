import json,os,subprocess,sys
from pathlib import Path
p=Path('/opt/validation042');c=json.loads((p/'private/browser.json').read_text())
env=dict(os.environ,NODE_PATH=str(p/'browser/node_modules'),CHROMIUM_PATH='/opt/validation042/chromium-app-netns.py',SETUP_E2E_CREDENTIALS=str(p/'private/browser.json'),SETUP_E2E_OUTPUT=str(p/'private/setup'),SETUP_E2E_RESULT=str(p/'evidence/model-lifecycle.json'),PROVIDER_E2E_URL=c['url'],PROVIDER_E2E_USER=c['user'],PROVIDER_E2E_PASSWORD=c['password'],PROVIDER_E2E_FIXTURE='http://127.0.0.1:18765',PROVIDER_E2E_PREFIX='release042',PROVIDER_E2E_ARTIFACTS=str(p/'private/providers'))
phase=sys.argv[1]
scripts={'setup':'browser-fresh.cjs','models':'models-stable.cjs','providers':'source/scripts/validation/providers/browser.cjs','after-restart':'source/scripts/validation/providers/browser.cjs','exploratory':'source/scripts/validation/providers/exploratory.cjs','module-down':'source/scripts/validation/providers/exploratory.cjs','navigation':'navigation.cjs'}
if phase in ('after-restart','module-down'):env['PROVIDER_E2E_PHASE']=phase
r=subprocess.run(['node',str(p/scripts[phase])],env=env,cwd=p)
raise SystemExit(r.returncode)
