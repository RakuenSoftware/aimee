#!/usr/bin/env python3
"""MR-01 common lifecycle fixture through advertised KB retrieval endpoints."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time
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
    def check(name, passed, elapsed_ms=None):
        row = dict(name=name, passed=bool(passed))
        if elapsed_ms is not None:
            row['elapsed_ms'] = round(elapsed_ms, 3)
        checks.append(row)
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
        if not passed:
            raise RuntimeError(name)
    env = dict(os.environ, AIMEE_RUNTIME_WEB_ENABLED='0', AIMEE_POSTGRES_VOLUME_MIB='512',
               COMPOSE_PROFILES='', EMBEDDER_MODEL='bekko-a25m',
               EMBEDDER_URL='https://aimee-embedder:8762', EMBEDDER_DIMS='384',
               AIMEE_PROVIDER_CONTEXT_LIMITS=json.dumps(dict(schema_version=1, max_request_bytes=32768)))
    key = 'mr01-lanes-' + uuid.uuid4().hex
    kb = matrix.Stack('kb', env, args.output)
    def sql(query):
        return matrix.command('docker', 'exec', kb.postgres, 'psql', '-U', 'postgres',
            '-d', 'aimee_store', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-c', query)
    def call(verb, body):
        started = time.monotonic()
        code, result = kb.kb_request('/v1/actions/memory.'+verb, body)
        return code, result, (time.monotonic()-started)*1000
    try:
        kb.start()
        with matrix.paused_relation_consumer(kb):
            states = ['current','future','expired','suppressed','superseded','archived',
                      'quarantined','deleted','revoked','cross-scope']
            values = []
            for state in states:
                life = state if state in ('superseded','archived','quarantined','deleted','revoked') else 'active'
                scope = key+'-foreign' if state == 'cross-scope' else key
                values.append(f"('L2','fact','{key}-{state}','{key} {state}','project','{scope}','{life}',{int(state=='suppressed')})")
            values.append(f"('L2','fact','{key}-workspace','{key} workspace','workspace','{key}-team','active',0)")
            ids = json.loads(sql("BEGIN; INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,activation_suppressed) VALUES "+','.join(values)+f""";
              UPDATE memories SET effectiveness=0.1 WHERE key LIKE '{key}-%';
              UPDATE memories SET valid_from=(now()+interval '1 day')::text WHERE key='{key}-future';
              UPDATE memories SET valid_until=(now()-interval '1 day')::text WHERE key='{key}-expired';
              INSERT INTO memory_provenance(memory_id,session_id,action,details,created_at)
                SELECT id,'fixture','observed',key,now()::text FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO memory_links(source_id,target_id,relation)
                SELECT c.id,m.id,'depends_on' FROM memories c CROSS JOIN memories m
                WHERE c.key='{key}-current' AND m.key LIKE '{key}-%' AND m.id<>c.id;
              INSERT INTO memory_conflicts(memory_a,memory_b,detected_at)
                SELECT c.id,m.id,now()::text FROM memories c CROSS JOIN memories m
                WHERE c.key='{key}-current' AND m.key LIKE '{key}-%' AND m.id<>c.id;
              INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity)
                SELECT id,'{key}-root','uses',key FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO entity_edges(source,relation,target)
                SELECT '{key}-root','calls',key FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance)
                SELECT e.id,'memory','memory:'||m.id::text,'supports' FROM entity_edges e JOIN memories m ON m.key=e.target
                WHERE e.source='{key}-root';
              UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index' AND document_id IN
                (SELECT id FROM memories WHERE key LIKE '{key}-%');
              SELECT json_object_agg(key,id) FROM memories WHERE key LIKE '{key}-%'; COMMIT"""))
            workspace_id = ids[key+'-workspace']
            ids = {state: ids[key+'-'+state] for state in states}
            for verb in ('find_facts','find_facts_visible','find_facts_scoped','list'):
                body = dict(query=key,scope_context=True,project=key,limit=64)
                if verb == 'find_facts_scoped':
                    body.update(scope_type='project',scope_value=key)
                code, result, elapsed = call(verb,body)
                records = result.get('memories' if verb == 'list' else 'facts', [])
                check('Common fixture lexical/list '+verb, code == 200 and result.get('status') == 'ok'
                      and {int(row['id']) for row in records} == {ids['current']}, elapsed)
            code,result,elapsed = call('search',dict(query=key,keywords=[key],view='server',
                scope_context=True,project=key,limit=32))
            check('Common fixture compatibility search and windows', code == 200 and result.get('status') == 'ok'
                  and {int(row['id']) for row in result.get('facts',[])} == {ids['current']}
                  and len(result.get('windows',[])) == 1, elapsed)
            for verb in ('top_l2_facts','load_eval_corpus','list_session_scope_priority',
                         'list_session_scope_priority_like','search_facts_patterns_by_keyword'):
                code,result,elapsed = call(verb,dict(scope_context=True,project=key,max=64,
                    pattern='%'+key+'%',keyword='%'+key+'%'))
                check('Common fixture query route '+verb, code == 200 and result.get('status') == 'ok'
                      and {int(row['id']) for row in result.get('memories',[])} == {ids['current']}, elapsed)
            code,result,elapsed = call('diagnose_scoped',dict(query=key,scope_type='project',scope_value=key,limit=64))
            check('Common fixture diagnostic retrieval', code == 200 and result.get('status') == 'ok'
                  and {int(row['memory']['id']) for row in result.get('rows',[])} == {ids['current']}, elapsed)
            code,result,elapsed = call('recall',dict(task_hint=key,project=key,limit_tokens=8192))
            check('Common fixture recall bundle with legacy project', code == 200 and result.get('status') == 'ok'
                  and {int(row['memory_id']) for row in result.get('recall',{}).get('active_context',[])} == {ids['current']}, elapsed)
            for audience,value,label in [('project',key,'current'),('workspace',key+'-team','workspace')]:
                code,result,elapsed = call('assemble_context',dict(task_hint=key,**{audience:value}))
                content = result.get('context','')
                check('Common fixture assembled '+audience+' context', code == 200 and result.get('status') == 'ok'
                      and key+' '+label in content
                      and not any(key+' '+state in content for state in states+['workspace'] if state != label), elapsed)
            for audience, value, expected in [('project',key,ids['current']),('workspace',key+'-team',workspace_id)]:
                code, result, elapsed = call('find_facts',dict(query=key,**{audience:value},limit=64))
                check('Legacy '+audience+' applies without scope_context', code == 200 and result.get('status') == 'ok'
                      and {int(row['id']) for row in result.get('facts',[])} == {expected}, elapsed)
            code,result,elapsed = call('find_facts',dict(query=key,include_all=False))
            check('Explicit include_all false restricts to shared audience', code == 200
                  and result.get('status') == 'ok' and result.get('facts') == [], elapsed)
            for field,value in [('project',{}),('workspace',None),('scope_context','yes'),('include_all',1)]:
                code,result,elapsed = call('find_facts',dict(query=key,**{field:value}))
                check('Malformed '+field+' cannot become all-scope retrieval', code == 200
                      and result.get('kind') == 'invalid_argument' and 'facts' not in result, elapsed)
            code, result, elapsed = call('ontology',dict(action='walk',entity=key+'-root',
                scope_context=True,project=key,hops=1))
            check('Common fixture graph-only evidence-backed traversal', code == 200 and result.get('status') == 'ok'
                  and {row['target'] for row in result.get('entries',[])} == {key+'-current'}, elapsed)
            for state in states:
                for mode in ('current','historical'):
                    body = dict(id=str(ids[state]),scope_context=True,project=key)
                    if mode == 'historical':
                        body['read_policy'] = dict(schema_version=1,mode='historical',valid_at='2020-01-01T00:00:00Z')
                    code, result, elapsed = call('get',body)
                    allowed = state == 'current' or (mode == 'historical' and state in ('expired','superseded','archived'))
                    check('Common fixture '+mode+' '+state, code == 200 and
                          ((result.get('status') == 'ok' and int(result['memory']['id']) == ids[state]) if allowed
                           else result.get('kind') == 'not_found' and 'memory' not in result), elapsed)
            for audience,value,expected in [('project',key,key+'-current'),('workspace',key+'-team',key+'-workspace')]:
                code,result,elapsed = call('query_edges',dict(entity=key+'-root',**{audience:value},max=64))
                check('Legacy graph query honors '+audience, code == 200 and result.get('status') == 'ok'
                      and {row['target'] for row in result.get('edges',[])} == {expected}, elapsed)
            for state in ('current','cross-scope'):
                code,result,elapsed = call('get_provenance',dict(memory_id=str(ids[state]),project=key))
                check('Legacy provenance audience '+state, code == 200 and result.get('status') == 'ok'
                      and len(result.get('entries',[])) == (1 if state == 'current' else 0), elapsed)
            code,result,elapsed = call('link_query',dict(memory_id=str(ids['current']),project=key,max=64))
            foreign_ids = {ids['cross-scope'],workspace_id}
            check('Legacy link audience excludes foreign endpoints', code == 200 and result.get('status') == 'ok'
                  and not any(int(row[k]) in foreign_ids for row in result.get('links',[]) for k in ('source_id','target_id')), elapsed)
            code,result,elapsed = call('list_conflicts',dict(project=key,max=256))
            check('Legacy conflict audience excludes foreign parents', code == 200 and result.get('status') == 'ok'
                  and not any(int(row[k]) in foreign_ids for row in result.get('conflicts',[]) for k in ('memory_a','memory_b')), elapsed)
            for audience,value,total,conflicts in [('project',key,9,8),('workspace',key+'-team',1,0)]:
                code,result,elapsed = call('stats',dict(**{audience:value}))
                stats=result.get('stats',{})
                check('Operator statistics honor '+audience+' without current-only filtering',
                      code == 200 and result.get('status') == 'ok' and stats.get('total') == total
                      and stats.get('conflicts') == conflicts, elapsed)
                code,result,elapsed = call('stats',dict(view='console',effectiveness=True,**{audience:value}))
                display=result.get('display',{})
                check('Console effectiveness retains '+audience+' restriction', code == 200
                      and result.get('status') == 'ok' and display.get('total') == total
                      and display.get('effectiveness',{}).get('low_effectiveness') == total, elapsed)
                code,result,elapsed = call('stats_dashboard',dict(**{audience:value}))
                scopes=result.get('dashboard',{}).get('scopes',[])
                check('Dashboard excludes hidden '+audience+' conflict endpoints', code == 200
                      and result.get('status') == 'ok' and sum(row['count'] for row in scopes) == total
                      and sum(row['conflicted_memories'] for row in scopes) == 2*conflicts, elapsed)
            card_id = int(sql(f"""BEGIN;
              INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
                VALUES('L1','episode','{key}-derived','{key}-derived copied current input','project','{key}');
              INSERT INTO memory_units(memory_id,unit_type,unit_key,unit_text,is_episode_card)
                SELECT id,'episode_card','fixture','derived current input',1 FROM memories WHERE key='{key}-derived';
              INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
                SELECT 'memory_unit',u.id,'episode-card-input-v1',jsonb_build_object(
                  'schema_version',1,'owner_id',o.owner_id::text,'parent_revision',m.record_revision::text,
                  'unit_digest',encode(sha256(convert_to(jsonb_build_array(u.unit_type,u.unit_key,u.unit_text,u.memory_kind,u.weight)::text,'UTF8')),'hex'),
                  'inputs',jsonb_build_array(jsonb_build_object('record_id',p.id::text,'record_revision',p.record_revision::text)))::text
                FROM memory_units u JOIN memories m ON m.id=u.memory_id
                CROSS JOIN memory_collection_owner o CROSS JOIN memories p
                WHERE m.key='{key}-derived' AND o.id=1 AND p.id={ids['current']};
              UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index' AND document_id IN
                (SELECT id FROM memories WHERE key='{key}-derived');
              SELECT id FROM memories WHERE key='{key}-derived'; COMMIT"""))
            for revoked in (False,True):
                if revoked:
                    sql(f"UPDATE memories SET lifecycle_state='revoked' WHERE id={ids['current']}")
                code,result,elapsed = call('recall',dict(task_hint=key+'-derived',project=key,limit_tokens=8192,
                    activation=dict(current_turn=100,rows=[])))
                expected = set() if revoked else {card_id}
                check('Activated derived card '+('refuses revoked input' if revoked else 'admits observed current input'),
                      code == 200 and result.get('status') == 'ok'
                      and {int(row['memory_id']) for row in result.get('recall',{}).get('active_context',[])} == expected, elapsed)
        identities = []
        for name in (kb.application,kb.postgres,kb.embedder):
            value = json.loads(matrix.command('docker','inspect',name))[0]
            identities.append(dict(name=name,image=value['Image'],configured_image=value['Config']['Image']))
        (args.output/'image-identities.json').write_text(json.dumps(identities,indent=2)+'\n')
    except (RuntimeError,OSError,ValueError,subprocess.SubprocessError) as error:
        checks.append(dict(name='common fixture completed',passed=False,error=str(error)))
    finally:
        (args.output/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')
        kb.compose('down','--volumes','--remove-orphans')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
