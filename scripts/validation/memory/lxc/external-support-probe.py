#!/usr/bin/env python3
"""Additional shipping API acceptance on already-provisioned external guests."""
import ast,hashlib,json,os,subprocess,sys,time
from pathlib import Path
backend=sys.argv[1];root=Path('/var/lib/aimee-memory-lab');out=root/'external-support-api';out.mkdir(exist_ok=True)
run=Path((root/'last-run').read_text().strip());client=run/'stack/client';remote=(client/'remote.conf').read_text().splitlines()
os.environ.update(SERVER_URL=remote[0],BEARER=remote[1],CLIENT_CERT=str(client/'tls/client.crt'),CLIENT_KEY=str(client/'tls/client.key'))
base=remote[0];checks=[];counter=0;run_tag=str(time.time_ns());project='external-support-'+run_tag
source=ast.parse(Path('/opt/aimee/scripts/aimee-memory-lxc-probe.py').read_text());exec(compile(ast.Module(body=[n for n in source.body if isinstance(n,ast.FunctionDef) and n.name in ('request','require','check','args','get','has')],type_ignores=[]),'helpers','exec'))
state=json.loads((root/'resume-private.json').read_text());token=state['kb-supervisor']['env'].get('AIMEE_KB_API_BEARER_TOKEN','')
def kb(verb,body):
 global counter
 r=subprocess.run(['curl','-sS','--max-time','150','-H','Authorization: Bearer '+token,'-H','Content-Type: application/json','--data-binary',json.dumps(body),'http://127.0.0.1:8741/v1/actions/memory.'+verb,'-w','\n%{http_code}'],capture_output=True,text=True)
 assert r.returncode==0
 raw,code=r.stdout.rsplit('\n',1);result=json.loads(raw);counter+=1
 (out/f'kb-{counter:04d}.json').write_text(json.dumps({'verb':verb,'http':int(code),'response':result},indent=2))
 assert int(code)==200,result
 return require(result)

def source_check():
 stored=require(request('memory/store',args('kb',key='Kibukx height '+run_tag,content='Kibukx person height 69cm; Virant message alpha',kind='fact')))
 recall=kb('recall',{'project':project,'scope_context':True,'task_hint':'Kibukx height','limit_tokens':8192})
 bundle=recall['recall'];source=bundle['collection_source']
 refs=[{'channel':'native_memory_collection','stable_id':'1','source_version':source}]
 check_id=hashlib.sha256(run_tag.encode()).hexdigest()[:32]
 payload={'project':project,'scope_context':True,'revalidation':{'schema_version':1,'check_id':check_id,'sources':refs}}
 result=kb('revalidate_sources',payload);assert result.get('eligible') is True,result
 payload['revalidation']['send_guard']='acquire'
 result=kb('revalidate_sources',payload);assert result.get('send_guard')=='acquired',result
 blocked=request('memory/store',args('kb',key='guarded-'+run_tag,content='must wait for lease',kind='fact'))
 assert blocked.get('status')=='error',blocked
 payload['revalidation']['send_guard']='release';payload['revalidation']['sources']=[]
 assert kb('revalidate_sources',payload).get('send_guard')=='released'
 require(request('memory/store',args('kb',key='after-guard-'+run_tag,content='new after lease',kind='fact')))
 payload['revalidation']['send_guard']='';payload['revalidation']['sources']=refs
 assert kb('revalidate_sources',payload).get('eligible') is False
check('shipping source revalidation, send guard, mutation refusal/release and stale collection',source_check)

def history():
 data=require(request('memory/store',args('kb',key='shared-history-'+run_tag,content='Kibukx height is 69cm',kind='fact')))
 current=get('kb',data['id'],include_version=True);version=current['memory']['version']
 require(request('memory/supersede',args('kb',id=data['id'],old_id=data['id'],new_content='Kibukx height is 170cm',confidence=1,expected_version=version)))
 result=kb('get',{'project':project,'scope_context':True,'id':data['id'],'at_version':version})
 assert has(result,'69cm'),result
check('shipping external shared exact historical revision',history)

def briefing():
 data=kb('briefing',{'project':project,'scope_context':True,'limit_tokens':8192,'format':'json'})
 assert has(data,'170cm'),data
check('shipping KB briefing contains corrected evidence',briefing)

def capabilities():
 data=require(request('commands/memory.backend_capabilities',{}))['result']
 assert data['version']==2 and data['profile']=='aimee-conversation-v2'
 assert data['limits']['max_candidates']==(16 if backend=='cognee' else 256) and data['retrieval_complete'] is False
check('shipping capabilities advertise profile and bounded non-exhaustive retrieval',capabilities)

(out/'summary.json').write_text(json.dumps({'backend':backend,'checks':checks,'status':'passed' if all(c['status']=='passed' for c in checks) else 'failed'},indent=2))
raise SystemExit(0 if all(c['status']=='passed' for c in checks) else 1)
