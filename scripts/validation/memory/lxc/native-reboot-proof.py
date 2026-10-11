#!/usr/bin/env python3
import ast,hashlib,json,os,re,subprocess,sys,time
from pathlib import Path
root=Path('/var/lib/aimee-memory-lab');out=root/'native-fixes';phase=sys.argv[1]
def sql(q):return subprocess.check_output(['runuser','-u','postgres','--','/usr/lib/postgresql/18/bin/psql','-X','-d','aimee_validation','-tAc',q],text=True).strip()
def controls():
 return {table:sql(f"SELECT md5(COALESCE(string_agg(memory_id::text||':'||payload_digest,',' ORDER BY memory_id,payload_digest),'')) FROM {table}") for table in ['aimee_private.user_memory_erasure_intents','public.memory_erasure_intents']}
if phase=='before':
 anchors={}
 for p in (root/'evidence').glob('api-*.json'):
  d=json.loads(p.read_text());r=d.get('response',{});m=r.get('memory',{})
  if d.get('route')=='memory/get' and r.get('status')=='ok' and '170cm' in m.get('content','') and not m.get('historical'):
   tag=re.search(r'fixture=(\d+)',m['content']);assert tag
   anchors[r['store']]={'id':m['id'],'content':m['content'],'project':'memory-lxc-'+tag[1]}
 assert set(anchors)=={'user','kb'}
 (out/'pre-reboot.json').write_text(json.dumps({'anchors':anchors,'erasure_hashes':controls()},indent=2));raise SystemExit(0)
backend='native';checks=[];counter=0;out=out/'reboot';out.mkdir(exist_ok=True)
run=Path((root/'last-run').read_text().strip());client=run/'stack/client';remote=(client/'remote.conf').read_text().splitlines();os.environ.update(SERVER_URL=remote[0],BEARER=remote[1],CLIENT_CERT=str(client/'tls/client.crt'),CLIENT_KEY=str(client/'tls/client.key'));base=remote[0]
source=ast.parse(Path('/opt/aimee/scripts/aimee-memory-lxc-probe.py').read_text());exec(compile(ast.Module(body=[n for n in source.body if isinstance(n,ast.FunctionDef) and n.name in ['request','require','check','has']],type_ignores=[]),'helpers','exec'))
prior=json.loads((root/'native-fixes/pre-reboot.json').read_text())
deadline=time.monotonic()+180
while True:
 try:
  if request('memory/list',{'store':'user','limit':1}).get('status')=='ok':break
 except Exception:pass
 assert time.monotonic()<deadline,'native guest did not become ready'
 time.sleep(2)
for store,a in prior['anchors'].items():
 def persisted(store=store,a=a):
  body={'store':store,'id':a['id'],'include_version':True}
  if store=='kb':body['project']=a['project']
  r=require(request('memory/get',body));assert r['memory']['content']==a['content'],r
 check(store+' exact corrected payload retained after full guest reboot',persisted)
def retained():assert controls()==prior['erasure_hashes'],'retained native erasure controls changed'
check('native SQL erasure intents retained after full guest reboot',retained)
(out/'summary.json').write_text(json.dumps({'checks':checks,'status':'passed' if all(c['status']=='passed' for c in checks) else 'failed'},indent=2))
raise SystemExit(0 if all(c['status']=='passed' for c in checks) else 1)
