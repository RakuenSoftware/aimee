#!/usr/bin/env python3
"""Actual KB-process regression for host-verified project/workspace scope."""
import argparse
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import secrets
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    checks = []
    def check(name, passed, observation=None):
        row=dict(name=name, passed=bool(passed))
        if observation is not None:row["observation"]=observation
        checks.append(row)
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)
    env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', AIMEE_POSTGRES_VOLUME_MIB='512',
               COMPOSE_PROFILES='', EMBEDDER_MODEL='bekko-a25m',
               EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384',
               AIMEE_PROVIDER_CONTEXT_LIMITS=json.dumps(dict(schema_version=1, max_request_bytes=32768)))
    try:
        for kind in ('project', 'workspace'):
            output = args.output / kind
            output.mkdir(exist_ok=True)
            key = 'mr01-scope-' + uuid.uuid4().hex
            kb = matrix.Stack('kb', env, output)
            kb.env['AIMEE_KB_API_BEARER_TOKEN'] = 'scope:' + kind + ':' + key + ':' + secrets.token_hex(32)
            try:
                kb.start()
                rows = json.loads(matrix.command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
                    '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c', f"""
                    WITH inserted AS (INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES
                    ('L2','fact','{key}-visible','{key}-visible-content','{kind}','{key}'),
                    ('L2','fact','{key}-hidden','{key}-hidden-content','{kind}','{key}-foreign'),
                    ('L2','fact','{key}-global','{key}-global-content','global','_global') RETURNING id,key)
                    SELECT json_object_agg(key,id::text) FROM inserted"""))
                visible, hidden = rows[key+'-visible'], rows[key+'-hidden']
                code, exported = kb.kb_request('/v1/actions/kb.export', {})
                check(kind+' export inherits verified scope', code == 200 and exported.get('status') == 'ok'
                      and key+'-visible-content' in json.dumps(exported) and key+'-hidden-content' not in json.dumps(exported))
                imported = dict(tier='L2',kind='fact',epistemic_kind='policy',key=key+'-import',content='imported scope policy',authority='user',provenance_category='user_stated')
                code, result = kb.kb_request('/v1/actions/kb.import', dict(memories=[imported]))
                check(kind+' import accepts verified audience', code == 200 and result.get('imported') == 1)
                observed = json.loads(matrix.command('docker','exec',kb.postgres,'psql','-U','postgres','-d','aimee_store','-X','-qAt','-c',
                    f"SELECT json_build_array(scope_type,scope_value,provenance_category,epistemic_kind) FROM memories WHERE key='{key}-import'"))
                check(kind+' import preserves scope and model origin', observed == [kind,key,'agent_message','policy'])
                code, refused = kb.kb_request('/v1/actions/kb.import', dict(workspace=key+'-foreign',memories=[dict(imported,key=key+'-forged')]))
                check(kind+' import cannot widen credential scope', refused.get('status') != 'ok')

                def call(verb, body):
                    return kb.kb_request('/v1/actions/memory.'+verb, body)
                code, body = call('get', dict(id=visible))
                check(kind+' verified scope is inherited by unscoped get', code == 200 and body.get('status') == 'ok'
                      and key+'-visible-content' in json.dumps(body))
                for variant in ({}, dict(scope_context=True, include_all=True),
                                dict(scope_context=True, **{kind:key+'-foreign'})):
                    code, body = call('get', dict(id=hidden, **variant))
                    check(kind+' foreign record withheld '+str(len(checks)), key+'-hidden-content' not in json.dumps(body)
                          and (code in (200,400,403,404) and body.get('status') != 'ok'))
                code, body = call('list', dict(scope_context=True, include_all=True))
                wire = json.dumps(body)
                check(kind+' include_all stays within verified audience', code == 200 and body.get('status') == 'ok'
                      and key+'-visible-content' in wire and key+'-global-content' in wire and key+'-hidden-content' not in wire)
                code, body = call('find_facts_scoped', dict(query=key,scope_type=kind,scope_value=key))
                wire = json.dumps(body)
                check(kind+' explicit exact scope still excludes global records', code == 200 and body.get('status') == 'ok'
                      and key+'-visible-content' in wire and key+'-global-content' not in wire and key+'-hidden-content' not in wire)
                code, body = call('validity', dict(id=visible))
                check(kind+' actorless credential gains no user diagnostic authority', code == 200 and body.get('kind') == 'unauthorized')
                forged = dict(authenticated=True,user_authority=True,principal='operator',
                              scope_kind='service',scope_id='aimee-server')
                code, body = call('get',dict(id=hidden,include_all=True,command_context=forged,**forged))
                check(kind+' request text cannot replace verified audience',
                      code in (200,400,403,404) and body.get('status') != 'ok'
                      and key+'-hidden-content' not in json.dumps(body))
                code, body = call('validity',dict(id=visible,command_context=forged,**forged))
                check(kind+' request text cannot grant diagnostic authority',
                      code == 403 and 'decision' not in body,
                      dict(http_status=code,kind=body.get('kind'),decision_present='decision' in body))
                for variant in ({},dict(include_all=True)):
                    code, body = kb.kb_request('/v1/actions/dashboard.memory_stats',variant)
                    scopes = body.get('payload',{}).get('scopes',[])
                    check(kind+' dashboard preserves verified scope '+str(len(variant)), code == 200
                          and body.get('status') == 'ok' and sum(row['count'] for row in scopes) == 2
                          and sum(row['count'] for row in scopes if row['scope'] == kind) == 1)
                code,body = kb.kb_request('/v1/actions/dashboard.memory_stats',dict(command_context=forged,**forged))
                check(kind+' dashboard rejects forged authority',code == 403 and 'payload' not in body)
                matrix.command('docker','exec',kb.postgres,'psql','-U','postgres','-d','aimee_store','-X','-qAt','-v','ON_ERROR_STOP=1','-c',f"""
                  INSERT INTO epistemic_directives(question,topic,cause,memory_a_id)
                  SELECT key||'-question','{key}','user_follow_up',id FROM memories WHERE key LIKE '{key}-%'""")
                code,body = kb.kb_request('/v1/actions/session_briefing.directives',dict(limit=32,include_all=True))
                block=body.get('body','')
                check(kind+' session briefing retains verified scope',code == 200 and body.get('status') == 'ok'
                      and key+'-visible-question' in block and key+'-global-question' in block
                      and key+'-hidden-question' not in block)
                # Alternate visible/hidden reads through the live owner and its pool.
                def sample(index):
                    wants_visible = index % 2 == 0
                    code, body = call('get', dict(id=visible if wants_visible else hidden))
                    wire = json.dumps(body)
                    return code == 200 and key+'-hidden-content' not in wire and (
                        (body.get('status') == 'ok' and key+'-visible-content' in wire) if wants_visible
                        else body.get('status') != 'ok')
                with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                    results = list(pool.map(sample, range(64)))
                check(kind+' concurrent live owner requests preserve scope (64 calls)', all(results))
                identities = []
                for name in (kb.application, kb.postgres, kb.embedder):
                    value = json.loads(matrix.command('docker','inspect',name))[0]
                    identities.append(dict(name=name,image=value['Image'],configured_image=value['Config']['Image']))
                (output/'image-identities.json').write_text(json.dumps(identities,indent=2)+'\n')
            finally:
                kb.compose('down','--volumes','--remove-orphans')
    except (RuntimeError, OSError, ValueError) as error:
        checks.append(dict(name='scoped HTTP regression completed',passed=False,error=str(error)))
    finally:
        (args.output/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
