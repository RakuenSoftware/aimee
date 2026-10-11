#!/bin/bash
set -euo pipefail
export PATH=/usr/local/go/bin:$PATH
backend=$1
if [[ $backend == hillock ]]; then
 python3 -m venv /opt/hillock-venv
 /opt/hillock-venv/bin/pip install -r /opt/aimee/integrations/hillock/requirements.txt
 git clone https://github.com/roandejager/Hillock.git /opt/hillock
 git -C /opt/hillock checkout --detach 1edd166ead75b85a9ab95cd6ba4faf7011ad567c
 git -C /opt/hillock rev-parse HEAD > /opt/hillock/AIMEE_UPSTREAM_REVISION
 install -d -m0700 /var/lib/aimee-provider
 openssl rand -hex 32 > /var/lib/aimee-provider/token
 chmod 0600 /var/lib/aimee-provider/token
 cat > /etc/systemd/system/aimee-hillock.service <<'UNIT'
[Unit]
Description=Pinned Hillock stateless retrieval for fresh Aimee validation
After=network.target
[Service]
Environment=HILLOCK_SOURCE=/opt/hillock
Environment=HILLOCK_TOKEN_FILE=/var/lib/aimee-provider/token
ExecStart=/opt/hillock-venv/bin/python /opt/aimee/integrations/hillock/service.py
Restart=on-failure
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
[Install]
WantedBy=multi-user.target
UNIT
 systemctl daemon-reload
 systemctl enable --now aimee-hillock
elif [[ $backend == cognee ]]; then
 python3 -m venv /opt/cognee-venv
 /opt/cognee-venv/bin/pip install 'cognee[api]==1.6.2'
 install -d -m0700 /var/lib/aimee-provider
 python3 - <<'PY'
import importlib.util,threading
spec=importlib.util.spec_from_file_location('fixture','/opt/aimee/scripts/validation/memory/run-cognee-contract.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
server=module.ThreadingHTTPServer(('127.0.0.1',8098),module.ModelFixture)
from pathlib import Path
Path('/var/lib/aimee-provider/fixture-start-check').write_text('fixture import checked')
PY
 cat > /var/lib/aimee-provider/model-fixture.py <<'PY'
import importlib.util
spec=importlib.util.spec_from_file_location('fixture','/opt/aimee/scripts/validation/memory/run-cognee-contract.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
module.ThreadingHTTPServer(('127.0.0.1',8098),module.ModelFixture).serve_forever()
PY
 python3 - <<'PY'
from pathlib import Path
import secrets
root=Path('/var/lib/aimee-provider')
password=secrets.token_hex(24)
(root/'account-password').write_text(password);(root/'account-password').chmod(0o600)
values={'DATA_ROOT_DIRECTORY':str(root/'data'),'SYSTEM_ROOT_DIRECTORY':str(root/'system'),'CACHE_ROOT_DIRECTORY':str(root/'cache'),'COGNEE_LOGS_DIR':str(root/'logs'),'COGNEE_REPOS_DIR':str(root/'repos'),'DEFAULT_USER_PASSWORD':password,'FASTAPI_USERS_JWT_SECRET':secrets.token_hex(32),'LLM_PROVIDER':'custom','LLM_MODEL':'openai/fixture','LLM_ENDPOINT':'http://127.0.0.1:8098/v1','LLM_API_KEY':'fixture','EMBEDDING_PROVIDER':'openai','EMBEDDING_MODEL':'text-embedding-3-small','EMBEDDING_ENDPOINT':'http://127.0.0.1:8098/v1','EMBEDDING_API_KEY':'fixture','EMBEDDING_DIMENSIONS':'8','COGNEE_TRACING_ENABLED':'false','TELEMETRY_DISABLED':'true'}
(root/'provider.env').write_text('\n'.join(f'{k}={v}' for k,v in values.items())+'\n');(root/'provider.env').chmod(0o600)
PY
 cat > /etc/systemd/system/aimee-cognee-model-fixture.service <<'UNIT'
[Unit]
Description=Deterministic local Cognee contract model fixture
[Service]
ExecStart=/usr/bin/python3 /var/lib/aimee-provider/model-fixture.py
Restart=on-failure
[Install]
WantedBy=multi-user.target
UNIT
 cat > /etc/systemd/system/aimee-cognee.service <<'UNIT'
[Unit]
Description=Real pinned Cognee API for fresh Aimee validation
After=network.target aimee-cognee-model-fixture.service
Requires=aimee-cognee-model-fixture.service
[Service]
WorkingDirectory=/var/lib/aimee-provider
EnvironmentFile=/var/lib/aimee-provider/provider.env
ExecStart=/opt/cognee-venv/bin/python -m uvicorn cognee.api.client:app --host 127.0.0.1 --port 8097
Restart=on-failure
[Install]
WantedBy=multi-user.target
UNIT
 systemctl daemon-reload
 systemctl enable --now aimee-cognee-model-fixture aimee-cognee
 python3 - <<'PY'
from pathlib import Path
import json,time,urllib.request,urllib.parse
base='http://127.0.0.1:8097'
for n in range(240):
 try:
  urllib.request.urlopen(base+'/openapi.json',timeout=2);break
 except OSError:time.sleep(1)
else:raise RuntimeError('Cognee API readiness timeout')
password=Path('/var/lib/aimee-provider/account-password').read_text().strip()
req=urllib.request.Request(base+'/api/v1/auth/login',data=urllib.parse.urlencode({'username':'default_user@example.com','password':password}).encode(),method='POST')
with urllib.request.urlopen(req,timeout=10) as r:session=json.load(r)['access_token']
req=urllib.request.Request(base+'/api/v1/auth/api-keys',data=json.dumps({'name':'aimee-memory-lxc-service'}).encode(),headers={'Authorization':'Bearer '+session,'Content-Type':'application/json'})
with urllib.request.urlopen(req,timeout=10) as r:credential=json.load(r)
token=credential['key']
id_path=Path('/var/lib/aimee-provider/service-key-id');id_path.write_text(credential['id']);id_path.chmod(0o600)
p=Path('/var/lib/aimee-provider/token');p.write_text(token);p.chmod(0o600)
print('Cognee authenticated; credential retained privately.')
PY
fi
