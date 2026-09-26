#!/usr/bin/env python3
"""Run the bounded MR-07 functional experiment on disposable Docker stacks.

Usage: python3 tests/e2e/memory-exploration-activation-e2e.py <image-head> <new-output-dir>
The image aimee-pr2990:<image-head> must exist. The controller seeds fixture
assertions and tenancy, then uses the actual authenticated native execution path.
The provider is synthetic: these checks are not paired quality/cost measurements.
Stacks remain available for evidence export and explicit project-scoped cleanup.
"""
import importlib.util,os,json,subprocess,time,hashlib,sys,shutil,re,datetime
from pathlib import Path
head=sys.argv[1];assert re.fullmatch('[a-f0-9]{9}',head)
root=Path(__file__).resolve().parents[2];os.chdir(root)
out=Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=False)
manifest=dict(schema_version=1,kind='functional_process_experiment',candidate=head,promotion=False,phases=['scope','coverage','activated','revoked'],limits=dict(enabled=True,raw_scans=0,starvation_turns=2),provider='synthetic; this is not a quality measurement')
raw=json.dumps(manifest,sort_keys=True,separators=(',',':')).encode();(out/'manifest.json').write_bytes(raw);manifest_hash=hashlib.sha256(raw).hexdigest()
spec=importlib.util.spec_from_file_location('matrix',root/'tests/e2e/deployment-matrix.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
spec=importlib.util.spec_from_file_location('placement',root/'tests/e2e/memory-placement-e2e.py');placement=importlib.util.module_from_spec(spec);spec.loader.exec_module(placement)
env=dict(os.environ,AIMEE_APPLICATION_IMAGE='aimee-pr2990:'+head,AIMEE_POSTGRES_IMAGE='aimee-pr2990-postgres:d0e3c5752',AIMEE_EMBEDDER_IMAGE='ghcr.io/rakuensoftware/aimee-embedder-a25m:0.4.5',AIMEE_RUNTIME_WEB_ENABLED='0',AIMEE_POSTGRES_VOLUME_MIB='512',COMPOSE_PROFILES='',EMBEDDER_MODEL='bekko-a25m',EMBEDDER_URL='https://aimee-embedder:8762',EMBEDDER_DIMS='384',AIMEE_PROVIDER_CONTEXT_LIMITS=json.dumps(dict(schema_version=1,max_request_bytes=65536)))
assert shutil.disk_usage('/').free>=8*1024**3
s=m.Stack('server',env,out);kb=m.Stack('kb',env,out)
s.network_override.write_text(s.network_override.read_text()+'    environment:\n      AIMEE_EXPLORATION_EXPERIMENT: "'+manifest_hash+'"\n')
def inside_json(path):
 p=subprocess.run(['docker','exec',s.application,'cat',path],capture_output=True,text=True)
 return json.loads(p.stdout) if p.returncode==0 else None
def write_inside(path,value,root_owned=False):
 data=json.dumps(value);code="from pathlib import Path;import sys,os;p=Path(sys.argv[1]);p.parent.mkdir(parents=True,exist_ok=True);p.write_text(sys.stdin.read());p.chmod(0o444 if sys.argv[2]=='root' else 0o600)"
 subprocess.run(['docker','exec','-i','-u','0' if root_owned else '1000',s.application,'python3','-c',code,path,'root' if root_owned else 'user'],input=data,text=True,check=True,stdout=subprocess.DEVNULL)
def sql(q):return m.command('docker','exec',kb.postgres,'psql','-U','postgres','-d','aimee_store','-X','-qAt','-v','ON_ERROR_STOP=1','-c',q)
def quote(value):return "'"+value.replace("'","''")+"'"
def read_contract(sid):
 assert re.fullmatch('[a-fA-F0-9-]{36}',sid)
 raw=m.command('docker','exec',s.postgres,'psql','-U','postgres','-d','aimee_store','-X','-qAt','-v','ON_ERROR_STOP=1','-c',"SELECT exploration_state FROM session_state WHERE session_id="+quote(sid))
 state=json.loads(raw);return state,state['tasks']['session-task']['revisions'][-1]
def check(name,ok):
 checks.append(dict(name=name,passed=bool(ok)));(out/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise RuntimeError(name)
checks=[];process=None
try:
 kb.start();m.command('docker','exec','-i','-u','1000',kb.application,'aimee-kb','--bootstrap-vault-stdin',data='AIMEE_KB_SERVICE_IDENTITY_TOKEN='+kb.service_identity+'\0');m.command('docker','restart',kb.application);kb.start();s.start()
 m.command('docker','network','connect','--alias','aimee-kb',s.project+'_default',kb.application)
 connection=m.command('docker','exec','-u','1000',kb.application,'aimee-kb','enroll','--host=aimee-kb','--port=8745','--scope=service:aimee-server')
 for key,value in [('kb_client_bearer_token',kb.env['AIMEE_KB_API_BEARER_TOKEN']),('kb_service_identity_token',kb.service_identity),('kb_connection_string',connection),('kb_mode','remote')]:
  reply=json.loads(m.command('docker','exec','-i',s.application,'python3','-c',placement.HTTP,data=json.dumps(dict(method='POST',path='/v1/config/set',body=dict(key=key,value=value)))))
  if reply[0]!=200 or reply[1].get('status')!='ok':raise RuntimeError('owned enrollment failed')
 m.command('docker','restart',s.application);s.start()
 (out/'names.json').write_text(json.dumps(dict(server=s.application,postgres=s.postgres,project=s.project,kb=kb.application,kb_project=kb.project)))
 write_inside('/var/lib/aimee/policy.json',dict(adaptive_exploration=manifest['limits']),True)
 subprocess.run(['docker','cp',str(root/'tests/e2e/memory-exploration-activation-inside.py'),s.application+':/tmp/mr07-activation-inside.py'],check=True,stdout=subprocess.DEVNULL)
 log=(out/'inside.log').open('w');process=subprocess.Popen(['docker','exec','-u','1000',s.application,'python3','/tmp/mr07-activation-inside.py'],stdout=log,stderr=subprocess.STDOUT)
 for phase in manifest['phases']:
  deadline=time.monotonic()+300
  while time.monotonic()<deadline:
   if process.poll() is not None:raise RuntimeError('fixture exited before '+phase)
   data=inside_json('/var/lib/aimee/mr07-activated-fixture/phase.json')
   if data and data.get('phase')==phase:break
   time.sleep(.25)
  else:raise RuntimeError('phase timed out')
  state,c=read_contract(data['session']);b=c['binding']
  if phase=='scope':
   check('host issues a clean owned worktree binding',b.get('host_worktree') and b.get('worktree_generation','').startswith('git-clean:'))
   key,project=data['key'],b['project'];qk,qp=quote(key),quote(project)
   code="from pathlib import Path;import json,sys;d=json.load(sys.stdin);p=Path(d['root']);p.mkdir(parents=True,exist_ok=True);(p/'state.c').write_text(d['source'])"
   subprocess.run(['docker','exec','-i','-u','0',kb.application,'python3','-c',code],input=json.dumps(dict(root=data['repository'],source=data['source'])),text=True,check=True)
   scan_id='mr07-activation-scan'
   for phase_name,extra in [('begin',dict(expected_files=1)),('stage',dict(files=[dict(rel_path='state.c',content=data['source'])])),('seal',dict(expected_files=1))]:
    status,response=kb.kb_request('/v1/code/scan',dict(project=project,root_path=data['repository'],phase=phase_name,scan_id=scan_id,**extra))
    check('canonical code scan '+phase_name,status in (200,202) and response.get('status') in ('ok','accepted'))
   username=m.command('docker','exec','-u','1000',s.application,'id','-un');assert re.fullmatch('[A-Za-z0-9_-]+',username)
   sql("BEGIN; INSERT INTO kb_team(id,name) VALUES(707,'mr07-functional-team'); INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES('aimee-server',707,1),("+quote(username)+",707,1); INSERT INTO kb_project(id,parent,name) VALUES(707,707,'mr07-functional-project'); UPDATE projects SET kb_project=707 WHERE name="+qp+"; INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,client_issuer,client_serial_norm,client_fingerprint) SELECT 'mr07-functional-server','service:aimee-server','mr07-functional-unused-mgmt',707,'https://aimee-server:8743','active',cert_issuer,cert_serial_norm,fingerprint FROM kb_enrollments WHERE scope='service:aimee-server' AND state='active' AND revoked_at='' LIMIT 1; COMMIT")
   identity_update="from pathlib import Path;import json;p=Path('/var/lib/aimee/kb-client-identity.json');d=json.loads(p.read_text());d.update(version=2,state='ready',host='aimee-kb',port=8745,server_id='mr07-functional-server',team_id=707);p.write_text(json.dumps(d));p.chmod(0o600)"
   m.command('docker','exec','-u','1000',s.application,'python3','-c',identity_update)
   sql(f"""BEGIN;
INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status) VALUES({qk},'assert','test:mr07-activation','system',100,'open');
INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,commit_id) VALUES({qk},'naming_convention','wrong-supplied-value','semantic','world_fact','persistent','A',.99,80,{qk});
INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,after_lifecycle,after_confidence,after_authority_rank,after_version) SELECT {qk},id,'assert',0,1,lifecycle_state,confidence,authority_rank,version FROM entity_edges WHERE commit_id={qk};
WITH p AS (INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact',{quote(key+'-parent')},'Deliberately wrong activation fixture claim','project',{qp}) RETURNING id)
INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance) SELECT e.id,'memory','memory:'||p.id::text,'supports' FROM entity_edges e,p WHERE e.commit_id={qk};
COMMIT""")
   # Exercise the authenticated Server route, not the controller's KB bearer.
   probe=dict(method='POST',path='/v1/index/investigate',body=dict(query='mr07activation_state',project=project,cwd=data['repository'],fallback=False))
   reply=json.loads(m.command('docker','exec','-i','-u','1000',s.application,'python3','-c',placement.HTTP,data=json.dumps(probe)))
   (out/'authorized-index-preflight.json').write_text(json.dumps(reply,indent=2)+'\n')
   rows=reply[1].get('results',[])
   check('authenticated code context has answerable current indexed source',reply[0]==200 and rows and rows[0].get('result',{}).get('answerability',{}).get('decision')=='answerable')
  elif phase=='coverage':
   check('live typed context is complete with current owner and index',c.get('coverage_complete') and c.get('query_class')=='typed_requirements' and b.get('index_observed_current') and b.get('owner_observed_current'))
   check('experiment raw scan allowance is explicitly zero',c['limits']==manifest['limits'])
   fields=['project','workspace','working_directory','worktree_generation','index_generation','route','provider','model','limits_digest','producer_build']
   scope={k:b[k] for k in fields};scope['query_class']=c['query_class']
   now=datetime.datetime.now(datetime.timezone.utc)
   artifact=dict(schema_version=1,kind='experiment',authorized_by='codex:pr2990-process-validation',created=now.isoformat(),expires=(now+datetime.timedelta(minutes=45)).isoformat(),manifest_sha256=manifest_hash,principal=state['principal'],sessions=[data['session']],scope=scope,limits=manifest['limits'])
   write_inside('/etc/aimee/exploration-experiment.json',artifact,True)
  elif phase=='activated':
   revisions=state['tasks']['session-task']['revisions']
   check('activated native work records experiment approval without claiming calibration',any(r.get('approval_kind')=='experiment' and r.get('calibration_receipt')=='experiment:'+manifest_hash for r in revisions))
   check('bounded recovery preserves the shared raw scan counter',state.get('usage',{}).get('raw_scans',0)==1)
   m.command('docker','exec','-u','0',s.application,'rm','/etc/aimee/exploration-experiment.json')
  else:check('revoked experiment returns new work to observe',c.get('tier')=='observe' and c.get('coverage_complete') and b.get('index_observed_current') and b.get('owner_observed_current'))
  started=time.monotonic();deadline=started+45;previous=None;stable_since=started;polls=0
  while time.monotonic()<deadline:
   row=json.loads(sql("SELECT json_build_object('pending',(SELECT count(*) FROM kb_async_jobs WHERE kind='memory_index' AND status<>'done'),'canonical',(SELECT COALESCE(sum(generation),0) FROM memory_collection_generations),'projection',(SELECT COALESCE(sum(generation),0) FROM memory_projection_generations))"));polls+=1
   if row!=previous or row['pending']!=0:previous=row;stable_since=time.monotonic()
   if row['pending']==0 and time.monotonic()-stable_since>=2:break
   time.sleep(.25)
  else:raise RuntimeError('frozen fixture indexing did not settle')
  (out/(phase+'-index-settlement.json')).write_text(json.dumps(dict(state=row,polls=polls,elapsed=time.monotonic()-started),indent=2)+'\n')
  write_inside('/var/lib/aimee/mr07-activated-fixture/continue.json',dict(phase=phase))
 code=process.wait(timeout=30);log.close();check('inside fixture actual process exit zero',code==0)
 subprocess.run(['docker','cp',s.application+':/var/lib/aimee/mr07-activated-fixture/checks.json',str(out/'inside-checks.json')],check=True,stdout=subprocess.DEVNULL)
 print('managed activation complete; owned stacks retained for evidence export',flush=True)
except Exception as exc:
 print('activation controller failure',type(exc).__name__,str(exc) if isinstance(exc,RuntimeError) else '', 'owned stacks retained',flush=True)
 if process is not None and process.poll() is None:process.terminate();process.wait(timeout=10)
 (out/'failure-type.txt').write_text(type(exc).__name__+'\n');raise SystemExit(1)
