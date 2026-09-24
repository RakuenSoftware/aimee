#!/usr/bin/env python3
"""MR-01 equivalent current decisions in actual Server and KB owners."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import types
import uuid

ROOT = Path(__file__).resolve().parents[2]
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tests/e2e'/file)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
matrix=load('matrix','deployment-matrix.py')
placement=load('placement','memory-placement-e2e.py')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    checks=[]
    def check(name,passed):
        checks.append(dict(name=name,passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ')+name,flush=True)
        if not passed:
            raise RuntimeError(name)
    env=dict(os.environ,AIMEE_RUNTIME_WEB_ENABLED='0',AIMEE_POSTGRES_VOLUME_MIB='512',
             COMPOSE_PROFILES='',EMBEDDER_MODEL='bekko-a25m',
             EMBEDDER_URL='https://aimee-embedder:8762',EMBEDDER_DIMS='384',
             AIMEE_PROVIDER_CONTEXT_LIMITS=json.dumps(dict(schema_version=1,max_request_bytes=32768)))
    stacks=[]
    try:
        kb=matrix.Stack('kb',env,args.output);stacks.append(kb);kb.start()
        matrix.command('docker','exec','-i','-u','1000',kb.application,'aimee-kb',
                       '--bootstrap-vault-stdin',data='AIMEE_KB_SERVICE_IDENTITY_TOKEN='+kb.service_identity+'\0')
        matrix.command('docker','restart',kb.application);kb.start()
        server=matrix.Stack('server',env,args.output);stacks.append(server);server.start()
        matrix.command('docker','network','connect','--alias','aimee-kb',server.project+'_default',kb.application)
        connection=matrix.command('docker','exec','-u','1000',kb.application,'aimee-kb','enroll',
                                  '--host=aimee-kb','--port=8745','--scope=service:aimee-server')
        if not connection.startswith('aimee://') or '\n' in connection:
            raise RuntimeError('invalid enrollment result')
        gate=placement.Gate(types.SimpleNamespace(server=server.application,store_db=server.postgres,kb=kb.application,kb_store_db=kb.postgres))
        for key,value in [('kb_client_bearer_token',kb.env['AIMEE_KB_API_BEARER_TOKEN']),
                          ('kb_service_identity_token',kb.service_identity),('kb_connection_string',connection),('kb_mode','remote')]:
            code,body=gate.call('/v1/config/set',dict(key=key,value=value))
            if code != 200 or body.get('status') != 'ok' or (key != 'kb_mode' and (body.get('value') is not True or body.get('secret') is not True)):
                raise RuntimeError('enrollment configuration failed')
        matrix.command('docker','restart',server.application);server.start()
        prefix='mr01-parity-'+uuid.uuid4().hex
        states=['current','expired','superseded','archived','quarantined','deleted','revoked','rejected','retired','unknown']
        observed=[]
        current_ids={}
        for state in states:
            lifecycle='active' if state in ('current','expired') else state
            private_until="now()-interval '1 day'" if state=='expired' else 'NULL'
            shared_until="(now()-interval '1 day')::text" if state=='expired' else 'NULL'
            private_id=gate.personal_sql(f"INSERT INTO user_memories(key,content,lifecycle_state,valid_until) VALUES('{prefix}-{state}','parity content','{lifecycle}',{private_until}) RETURNING id").splitlines()[0]
            shared_id=gate.sql(f"INSERT INTO memories(key,content,scope_type,scope_value,lifecycle_state,valid_until) VALUES('{prefix}-{state}','parity content','project','{prefix}','{lifecycle}',{shared_until}) RETURNING id").splitlines()[0]
            responses=[]
            for store,mid in [('user',private_id),('kb',shared_id)]:
                if state=='current':current_ids[store]=mid
                request=dict(store=store,id=mid)
                if store=='kb':request['project']=prefix
                code,read=gate.call('get',dict(request,include_version=True))
                status,raw=gate.call('validity',dict(request,mode='current'))
                decision=raw.get('decision',{})
                allowed=state=='current'
                check(store+' serving/diagnostic agreement '+state,status==200 and raw.get('status')=='ok'
                      and decision.get('eligible')==allowed and ((code==200 and read.get('status')=='ok') if allowed
                          else code==404 and read.get('kind')=='not_found' and 'memory' not in read))
                version=decision.get('checked_version',{})
                check(store+' exact owner/revision observation '+state,version.get('record_id')==mid
                      and bool(version.get('owner_id')) and bool(version.get('record_revision'))
                      and (not allowed or read.get('memory',{}).get('version')==version))
                responses.append(decision)
            fields=['schema_version','policy_version','mode','eligible','reason_codes','lifecycle',
                    'temporal_applicability','evidence_state','authority_class']
            left={k:responses[0].get(k) for k in fields};right={k:responses[1].get(k) for k in fields}
            check('Equivalent Server/KB domain decision '+state,left==right
                  and responses[0]['checked_version']['owner_id']!=responses[1]['checked_version']['owner_id'])
            observed.append(dict(state=state,server=left,kb=right))
        for store in ('user','kb'):
            for verb,field in [('list','memories'),('search','facts'),('recall','active_context')]:
                request=dict(store=store,query=prefix,limit=32)
                if verb=='search':request['keywords']=[prefix]
                if verb=='recall':request.update(task_hint=prefix,limit_tokens=8192)
                if store=='kb':request['project']=prefix
                code,result=gate.call(verb,request)
                rows=result.get('recall',{}).get(field,[]) if verb=='recall' else result.get(field,[])
                fixture_rows=[row for row in rows if row.get('key','').startswith(prefix+'-')]
                check(store+' common lifecycle fixture '+verb,code==200 and result.get('status')=='ok'
                      and {str(row['memory_id'] if verb=='recall' else row['id']) for row in fixture_rows}=={current_ids[store]})
        (args.output/'domain-decisions.json').write_text(json.dumps(observed,indent=2)+'\n')
        identities=[]
        for stack in stacks:
            for name in (stack.application,stack.postgres,stack.embedder):
                value=json.loads(matrix.command('docker','inspect',name))[0]
                identities.append(dict(name=name,image=value['Image'],configured_image=value['Config']['Image']))
        (args.output/'image-identities.json').write_text(json.dumps(identities,indent=2)+'\n')
    except (RuntimeError,OSError,ValueError,subprocess.SubprocessError) as error:
        checks.append(dict(name='decision parity completed',passed=False,error=str(error)))
    finally:
        for stack in reversed(stacks):stack.compose('down','--volumes','--remove-orphans')
        cleanup={stack.project:matrix.command('docker','network','ls','--filter',
            'label=com.docker.compose.project='+stack.project,'--format','{{.Name}}').splitlines() for stack in stacks}
        (args.output/'cleanup-networks.json').write_text(json.dumps(cleanup,indent=2)+'\n')
        if any(cleanup.values()):checks.append(dict(name='owned networks removed',passed=False))
        (args.output/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__=='__main__':
    raise SystemExit(main())
