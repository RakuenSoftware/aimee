#!/usr/bin/env python3
"""MR-01 common lifecycle fixture through advertised KB retrieval endpoints."""
import argparse
import contextlib
import hashlib
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
        result = subprocess.run(['docker','exec',kb.postgres,'psql','-U','postgres',
            '-d','aimee_store','-X','-qAt','-v','ON_ERROR_STOP=1','-c',query],
            text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=60)
        if result.returncode:
            # Only this synthetic SQL fixture is diagnosed; bootstrap commands
            # retain the matrix helper's credential-safe error handling.
            reason=result.stderr.splitlines()[0] if result.stderr else 'no diagnostic'
            raise RuntimeError('fixture SQL failed: '+reason)
        return result.stdout.strip()
    def call(verb, body):
        started = time.monotonic()
        code, result = kb.kb_request('/v1/actions/memory.'+verb, body)
        return code, result, (time.monotonic()-started)*1000
    try:
        kb.start()
        with contextlib.ExitStack() as fixture_guards:
            fixture_guards.enter_context(matrix.paused_relation_consumer(kb))
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
            for audience,value,wanted in [('project',key,ids['current']),('workspace',key+'-team',workspace_id)]:
                code,result,elapsed=call('ask',dict(query=key,**{audience:value},limit=8))
                check('Answer evidence retains '+audience+' current parents',code == 200 and result.get('status') == 'ok'
                      and set(result.get('evidence_trace',{}).get('candidate_ids',[])) == {wanted}
                      and set(result.get('citation_ids',[])).issubset({wanted}),elapsed)
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
                code,result = kb.kb_request('/v1/actions/dashboard.memory_stats',dict(**{audience:value}))
                scopes=result.get('payload',{}).get('scopes',[])
                check('Dashboard transport retains '+audience+' and authorized endpoints', code == 200
                      and result.get('status') == 'ok' and sum(row['count'] for row in scopes) == total
                      and sum(row['conflicted_memories'] for row in scopes) == 2*conflicts)
            sql(f"""INSERT INTO epistemic_directives(question,topic,cause,priority,memory_a_id)
              SELECT '{key}-question-'||key,'{key}','user_follow_up',
                CASE WHEN key IN ('{key}-current','{key}-workspace') THEN 1 ELSE 100 END,id
              FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO epistemic_directives(question,topic,cause,priority,memory_a_id,memory_b_id,resolution_memory_id)
              VALUES ('{key}-foreign-second','{key}','user_follow_up',100,{ids['current']},{ids['cross-scope']},0),
                ('{key}-foreign-resolution','{key}','user_follow_up',100,{ids['current']},0,{ids['cross-scope']}),
                ('{key}-authored','{key}','user_follow_up',0,0,0,0)""")
            for audience,value,state in [('project',key,'current'),('workspace',key+'-team','workspace')]:
                for mode,hint in [('matched',key),('fallback','unmatched-'+uuid.uuid4().hex)]:
                    code,result,elapsed = call('recall',dict(task_hint=hint,limit_tokens=8192,**{audience:value}))
                    questions = {row['question'] for row in result.get('recall',{}).get('directives',[])}
                    check('Directive '+mode+' recall requires every current '+audience+' parent',
                          code == 200 and result.get('status') == 'ok'
                          and questions == {key+'-question-'+key+'-'+state,key+'-authored'}, elapsed)
                code,result = kb.kb_request('/v1/actions/session_briefing.directives',dict(**{audience:value},limit=32))
                body=result.get('body','')
                wanted=key+'-question-'+key+'-'+state
                check('Session directive briefing retains '+audience+' parent scope', code == 200
                      and result.get('status') == 'ok' and wanted in body and key+'-authored' in body
                      and not any(key+'-question-'+key+'-'+other in body for other in states+['workspace'] if other != state)
                      and key+'-foreign-second' not in body and key+'-foreign-resolution' not in body)
            # Mandatory rule additions also invalidate an originally empty set.
            def rule_refs():
                code,result,_=call('recall',dict(task_hint=key,limit_tokens=8192,project=key))
                bundle=result.get('recall',{})
                if code != 200 or result.get('status') != 'ok':
                    raise RuntimeError('rule source recall unavailable')
                return [dict(channel='native_rule_collection',stable_id='1',source_version=bundle.get('rule_collection_source'))]+[
                    dict(channel='native_rules',stable_id=str(row['id']),source_version=row.get('source_version'))
                    for row in bundle.get('always_on_rules',[])]
            def rules_eligible(refs):
                code,result,_=call('revalidate_sources',dict(project=key,revalidation=dict(
                    schema_version=1,check_id=uuid.uuid4().hex,sources=refs)))
                if code != 200 or result.get('status') != 'ok':
                    raise RuntimeError('rule revalidation unavailable')
                return result.get('eligible')
            prior_rules=rule_refs()
            check('Hard rule collection is observed before insertion',prior_rules[0]['source_version'] is not None and rules_eligible(prior_rules) is True)
            rule_id=int(sql(f"INSERT INTO rules(polarity,title,description,directive_type,created_at,updated_at) VALUES('must','{key}-required','Never omit this constraint','hard',now()::text,now()::text) RETURNING id"))
            check('New mandatory rule invalidates earlier recall',rules_eligible(prior_rules) is False)
            selected_rules=rule_refs()
            check('Hard rule rows and collection have current observations',rules_eligible(selected_rules) is True
                  and any(ref['stable_id'] == str(rule_id) and ref['channel'] == 'native_rules' for ref in selected_rules))
            sql(f"UPDATE rules SET description='Changed constraint' WHERE id={rule_id}; UPDATE rules SET description='Never omit this constraint' WHERE id={rule_id}")
            check('Hard rule edit and restoration invalidate earlier selection',rules_eligible(selected_rules) is False)
            sql(f"DELETE FROM rules WHERE id={rule_id}")
            # Structured native rows carry the selected revisions and parent observations.
            reminder_id=int(sql(f"INSERT INTO prospective_memories(trigger_text,action_text,recurrence) VALUES('{key}','{key}-action','repeat') RETURNING id"))
            for audience,value,parent in [('project',key,ids['current']),('workspace',key+'-team',workspace_id)]:
                def structured_refs():
                    code,result,_=call('recall',dict(task_hint=key,limit_tokens=8192,**{audience:value}))
                    if code != 200 or result.get('status') != 'ok':
                        raise RuntimeError('structured source recall unavailable')
                    bundle=result.get('recall',{})
                    return [dict(channel=channel,stable_id=str(row['id']),source_version=row.get('source_version'))
                            for field,channel in [('directives','native_directives'),('reminders','native_reminders')]
                            for row in bundle.get(field,[])]
                refs=structured_refs()
                check('Structured '+audience+' recall observes roots and exact parents',len(refs) == 3
                      and all(ref['source_version'] and ref['source_version'].get('memory_parent_state') == 'observed'
                              and int(ref['source_version']['version']['record_revision']) > 0 for ref in refs)
                      and {int(p['record_id']) for ref in refs for p in ref['source_version'].get('memory_parents',[])} == {parent})
                def eligible(refs):
                    code,result,_=call('revalidate_sources',dict(**{audience:value},revalidation=dict(
                        schema_version=1,check_id=uuid.uuid4().hex,sources=refs)))
                    if code != 200 or result.get('status') != 'ok':
                        raise RuntimeError('structured source revalidation unavailable')
                    return result.get('eligible')
                check('Structured '+audience+' recall survives surfacing counters',eligible(refs) is True)
                directive_id=int(next(ref['stable_id'] for ref in refs if ref['source_version'].get('memory_parents')))
                question=sql(f"SELECT question FROM epistemic_directives WHERE id={directive_id}")
                sql(f"UPDATE epistemic_directives SET question='{key}-changed' WHERE id={directive_id}")
                check('Structured '+audience+' directive edit refuses earlier selection',eligible(refs) is False)
                sql(f"UPDATE epistemic_directives SET question='{question}' WHERE id={directive_id}")
                check('Structured '+audience+' directive restore cannot revive earlier revision',eligible(refs) is False)
                refs=structured_refs()
                check('Structured '+audience+' directive refresh admits a new decision',eligible(refs) is True)
                sql(f"UPDATE prospective_memories SET action_text='{key}-changed-action' WHERE id={reminder_id}")
                check('Structured '+audience+' reminder edit refuses earlier selection',eligible(refs) is False)
                sql(f"UPDATE prospective_memories SET action_text='{key}-action' WHERE id={reminder_id}")
                check('Structured '+audience+' reminder restore cannot revive earlier revision',eligible(refs) is False)
                check('Structured '+audience+' reminder refresh admits a new decision',eligible(structured_refs()) is True)
                refs=structured_refs()
                guard_id=uuid.uuid4().hex
                def guard(mode, selected=refs):
                    return call('revalidate_sources',dict(**{audience:value},revalidation=dict(
                        schema_version=1,check_id=guard_id,sources=None if mode == "release" else selected,send_guard=mode)))
                code,admission,elapsed=guard('acquire')
                check('Send guard '+audience+' durably acquires current sources',code == 200
                      and admission.get('eligible') is True and admission.get('send_guard') == 'acquired'
                      and admission.get('lease_ms') == 5000 and admission.get('guard_schema_version') == 2,elapsed)
                try:
                    refused=False
                    try:
                        sql(f"UPDATE epistemic_directives SET question='{key}-guarded-edit' WHERE id={directive_id}")
                    except RuntimeError as error:
                        refused='memory provider send in progress' in str(error)
                    check('Send guard '+audience+' blocks edit after admission',refused)
                    sql(f"UPDATE epistemic_directives SET surfaced_count=surfaced_count+1 WHERE id={directive_id}")
                    check('Send guard '+audience+' preserves recall accounting',eligible(refs) is True)
                    time.sleep(5.1)
                    refused=False
                    try:
                        sql(f"UPDATE epistemic_directives SET question='{key}-late-edit' WHERE id={directive_id}")
                    except RuntimeError as error:
                        refused='memory provider send in progress' in str(error)
                    check('Send guard '+audience+' deadline does not unlock an unresolved send',refused)
                    if audience == 'workspace':
                        # No provider dispatch is initiated by this admission-only fixture.
                        # Kill the authenticated owner, restart storage, and verify that
                        # neither event guesses an unresolved send completed.
                        subprocess.run(['docker','stop','--time','1',kb.application],check=True,stdout=subprocess.DEVNULL)
                        subprocess.run(['docker','restart',kb.postgres],check=True,stdout=subprocess.DEVNULL)
                        deadline=time.monotonic()+90
                        while time.monotonic()<deadline:
                            try:
                                if sql('SELECT 1') == '1': break
                            except RuntimeError:
                                pass
                            time.sleep(1)
                        check('Unresolved guard survives owner termination and database restart',
                              sql(f"SELECT count(*) FROM memory_send_leases WHERE token='{guard_id}'") == '1')
                        refused=False
                        try: sql(f"UPDATE epistemic_directives SET question='{key}-restart-race' WHERE id={directive_id}")
                        except RuntimeError as error: refused='memory provider send in progress' in str(error)
                        check('Restart does not admit protected mutation',refused)
                        # Verify termination before explicit storage-owner recovery.
                        check('Recovery verifies sending owner is stopped',subprocess.check_output(
                            ['docker','inspect','--format','{{.State.Running}}',kb.application],text=True).strip() == 'false')
                        sql(f"SELECT memory_send_guard_end('{guard_id}')")
                        fixture_guards.enter_context(matrix.paused_relation_consumer(kb))
                        subprocess.run(['docker','start',kb.application],check=True,stdout=subprocess.DEVNULL)
                        deadline=time.monotonic()+180
                        while time.monotonic()<deadline:
                            health=subprocess.check_output(['docker','inspect','--format','{{.State.Health.Status}}',kb.application],text=True).strip()
                            if health=='healthy': break
                            time.sleep(2)
                        check('Owner resumes after explicit orphan recovery',health=='healthy')

                finally:
                    code,released,elapsed=guard('release')
                check('Send guard '+audience+' releases before later work',code == 200
                      and released.get('send_guard') == 'released',elapsed)
                sql(f"UPDATE epistemic_directives SET question='{key}-guarded-edit' WHERE id={directive_id}")
                check('Send guard '+audience+' later edit invalidates earlier decision',eligible(refs) is False)
                guard_id=uuid.uuid4().hex
                code,stale,elapsed=guard('acquire')
                check('Send guard '+audience+' refuses stale reacquisition',code == 200
                      and stale.get('eligible') is False and 'send_guard' not in stale,elapsed)
                sql(f"UPDATE epistemic_directives SET question='{question}' WHERE id={directive_id}")
                check('Send guard '+audience+' refusal rolls back its lease',eligible(structured_refs()) is True)
                (args.output/('send-guard-'+audience+'.json')).write_text(json.dumps(dict(
                    sources=refs,admission=admission,release=released,stale_reacquisition=stale),indent=2)+'\n')
            # Reuse the same parent population for derived and retained-version views.
            scenes=json.loads(sql(f"""BEGIN;
              INSERT INTO memory_episodes(memory_id,episode_key,episode_text,source_session)
                SELECT id,key,key||' episode','{key}' FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO memory_entities(memory_id,entity)
                SELECT id,'{key}-root' FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO memory_scenes(workspace_id)
                SELECT key FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO memory_scene_members(scene_id,memory_id)
                SELECT s.id,m.id FROM memory_scenes s JOIN memories m ON m.key=s.workspace_id
                WHERE m.key LIKE '{key}-%';
              SELECT json_object_agg(workspace_id,id) FROM memory_scenes WHERE workspace_id LIKE '{key}-%'; COMMIT"""))
            for state in states:
                code,result,elapsed=call('get_episode',dict(episode_key=key+'-'+state,project=key))
                check('Derived episode parent '+state,code == 200 and
                      ((result.get('status') == 'ok' and int(result['episode']['memory_id']) == ids[state])
                       if state == 'current' else result.get('kind') == 'not_found' and 'episode' not in result),elapsed)
                code,result,elapsed=call('explain_match',dict(query=key,memory_id=str(ids[state]),project=key))
                check('Diagnostic match explanation current parent '+state,code == 200 and
                      ((result.get('status') == 'ok' and int(result['row']['memory']['id']) == ids[state])
                       if state == 'current' else result.get('kind') in ('not_found','unavailable') and 'row' not in result),elapsed)
                code,result,elapsed=call('scene_show',dict(scene_id=scenes[key+'-'+state],project=key))
                check('Derived scene members parent '+state,code == 200 and result.get('status') == 'ok'
                      and {int(row['memory_id']) for row in result.get('members',[])} == ({ids[state]} if state == 'current' else set()),elapsed)
                code,result,elapsed=call('fact_history',dict(key=key+'-'+state,project=key,max=1))
                allowed=state in ('current','future','expired','superseded','archived')
                check('Retained fact history audience and erasure '+state,code == 200 and result.get('status') == 'ok'
                      and {int(row['id']) for row in result.get('history',[])} == ({ids[state]} if allowed else set()),elapsed)
            for audience,value,state,parent in [('project',key,'current',ids['current']),('workspace',key+'-team','workspace',workspace_id)]:
                for verb,field,extra in [('entity_edges','edges',dict(entity=key+'-root')),
                                         ('search_graph','relations',dict(query=key))]:
                    code,result,elapsed=call(verb,dict(**extra,**{audience:value},limit=64))
                    check('Derived '+verb+' current '+audience+' parents',code == 200 and result.get('status') == 'ok'
                          and {int(row['memory_id']) for row in result.get(field,[])} == {parent},elapsed)
                code,result,elapsed=call('scene_list',dict(**{audience:value},limit=64))
                check('Derived scene list current '+audience+' parents',code == 200 and result.get('status') == 'ok'
                      and {int(row['id']) for row in result.get('scenes',[])} == {scenes[key+'-'+state]},elapsed)
                code,result,elapsed=call('entity_profile',dict(entity=key+'-root',**{audience:value}))
                profile=result.get('profile',{})
                check('Derived entity profile current '+audience+' aggregates',code == 200 and result.get('status') == 'ok'
                      and profile.get('mention_count') == 1 and profile.get('relation_count') == 1,elapsed)
                code,result,elapsed=call('briefing',dict(**{audience:value},limit_tokens=8192))
                briefing=result.get('briefing',{})
                check('Briefing facts activity and entities retain '+audience+' parents',code == 200 and result.get('status') == 'ok'
                      and {int(row['memory_id']) for row in briefing.get('key_facts',[])} == {parent}
                      and {row['summary'] for row in briefing.get('recent_activity',[])} == {key+'-'+state+' episode'}
                      and {(row['name'],row['mentions']) for row in briefing.get('active_entities',[])} == {(key+'-root',1)},elapsed)
            for interval,from_sql,until_sql in [('future',"(now()+interval '1 day')::text","''"),
                                                   ('expired',"''","(now()-interval '1 day')::text")]:
                sql(f"UPDATE memory_relations SET valid_at={from_sql},invalid_at={until_sql} WHERE memory_id={ids['current']}")
                for verb,field,extra in [('entity_edges','edges',dict(entity=key+'-root')),
                                         ('search_graph','relations',dict(query=key))]:
                    code,result,elapsed=call(verb,dict(**extra,project=key,limit=64))
                    check('Relation '+interval+' interval excludes '+verb,code == 200
                          and result.get('status') == 'ok' and result.get(field) == [],elapsed)
                code,result,elapsed=call('entity_profile',dict(entity=key+'-root',project=key))
                check('Relation '+interval+' interval excludes profile aggregate',code == 200
                      and result.get('status') == 'ok' and result.get('profile',{}).get('relation_count') == 0,elapsed)
            sql(f"UPDATE memory_relations SET valid_at='2020-01-01T09:00:00+09:00',invalid_at='2020-01-02T09:00:00+09:00' WHERE memory_id={ids['current']}")
            for at,wanted in [('2020-01-01T00:00:00Z',{ids['current']}),('2020-01-02T00:00:00Z',set())]:
                code,result,elapsed=call('search_graph_as_of',dict(query=key,project=key,as_of=at))
                check('Relation historical offset instant '+at,code == 200 and result.get('status') == 'ok'
                      and {int(row['memory_id']) for row in result.get('relations',[])} == wanted,elapsed)
            code,result,elapsed=call('search_graph_as_of',dict(query=key,project=key,as_of='now'))
            check('Relation historical query rejects relative time',code == 200 and result.get('kind') == 'invalid_argument',elapsed)
            sql(f"UPDATE memory_relations SET valid_at='',invalid_at='' WHERE memory_id={ids['current']}")
            sql(f"""BEGIN;
              INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
                VALUES('{key}','assert','fixture','system',100,'open');
              INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,commit_id)
                SELECT '{key}-root','uses',key,'semantic','world_fact','persistent','A',.9,80,'{key}'
                FROM memories WHERE key LIKE '{key}-%';
              INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,
                  after_lifecycle,after_confidence,after_authority_rank,after_version)
                SELECT '{key}',id,'assert',0,1,'persistent',confidence,80,1
                FROM entity_edges WHERE commit_id='{key}';
              INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance)
                SELECT e.id,'memory','memory:'||m.id::text,'supports' FROM entity_edges e JOIN memories m ON e.target=m.key
                WHERE e.commit_id='{key}'; COMMIT""")
            for audience,value,state in [('project',key,'current'),('workspace',key+'-team','workspace')]:
                expected=key+'-'+state
                code,result,elapsed=call('search_assertions',dict(query=key,**{audience:value},limit=64))
                check('Semantic assertion search retains '+audience+' parents',code == 200 and result.get('status') == 'ok'
                      and {row['object'] for row in result.get('assertions',[])} == {expected},elapsed)
                for verb,field in [('facts','facts'),('context_block','block')]:
                    code,result,elapsed=call(verb,dict(query=key+'-root',**{audience:value}))
                    (args.output/(verb+'-'+audience+'.json')).write_text(json.dumps(result,indent=2)+'\n')
                    content=result.get(field,'')
                    check('Semantic '+verb+' retains '+audience+' parents',code == 200 and result.get('status') == 'ok'
                          and expected in content and not any(key+'-'+other in content for other in states+['workspace'] if other != state),elapsed)
                    if verb == 'facts':
                        projection=result.get('fact_projection',{})
                        refs=projection.get('retained_items',[])
                        parent=ids['current'] if audience == 'project' else workspace_id
                        check('Fact projection observes only '+audience+' source versions',len(refs) == 1
                              and projection.get('source_version_state') == 'record_versions_observed'
                              and projection.get('projection_digest') == 'sha256:'+hashlib.sha256(content.encode()).hexdigest()
                              and projection.get('rendered_bytes') == len(content.encode())
                              and {int(p['record_id']) for ref in refs for p in ref['source_version']['memory_parents']} == {parent})
                code,result,elapsed=call('assemble_typed_context',dict(query=key,**{audience:value},enable_episodes=True,
                    channel_budgets=dict(total=8192,current_assertions=2048,episodes=2048)))
                channels=result.get('channels',{})
                (args.output/('typed-'+audience+'.json')).write_text(json.dumps(result,indent=2)+'\n')
                check('Typed assertion and episode channels retain '+audience+' parents',code == 200 and result.get('status') == 'ok'
                      and {row['object'] for row in channels.get('current_assertions',{}).get('items',[])} == {expected}
                      and {row['episode_key'] for row in channels.get('episodes',{}).get('items',[])} == {expected},elapsed)
                refs=result.get('retained_items',[])
                check('Typed projection observes only '+audience+' source versions',len(refs) == 2
                      and result.get('source_version_state') == 'record_versions_observed'
                      and {ref['channel'] for ref in refs} == {'current_assertions','episodes'}
                      and all(ref['source_version'].get('memory_parent_state') == 'observed' for ref in refs)
                      and {int(p['record_id']) for ref in refs for p in ref['source_version']['memory_parents']} == {parent})
            for audience,value in [('project',key),('workspace',key+'-team')]:
                code,result,elapsed=call('alerts',dict(**{audience:value}))
                alerts=result.get('alerts',{})
                wanted={ids[state] for state in ('future','expired','superseded','archived')} if audience == 'project' else set()
                (args.output/('alerts-'+audience+'.json')).write_text(json.dumps(result,indent=2)+'\n')
                check('Operator alerts retain '+audience+' history without erased parents',code == 200
                      and result.get('status') == 'ok'
                      and {row['memory_b_id'] for row in alerts.get('unresolved_contradictions',[])} == wanted
                      and {row['memory_id'] for row in alerts.get('newly_superseded',[])} == ({ids['superseded']} if audience == 'project' else set()),elapsed)
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
            # Replay the same lifecycle/scope population through every native
            # canonical section, after the graph/fact fixture has finished.
            for section,kind,prefix in [('identity','fact','identity:'),('preferences','preference','preference:'),('open_commitments','fact','commitment:')]:
                native_key=prefix+key+'-'+section
                native_values=[]
                for state in states+['workspace']:
                    life=state if state in ('superseded','archived','quarantined','deleted','revoked') else ('pending' if section=='open_commitments' else 'active')
                    scope_type='workspace' if state=='workspace' else 'project'
                    scope_value=key+'-team' if state=='workspace' else key+'-foreign' if state=='cross-scope' else key
                    native_values.append(f"('L2','{kind}','{native_key}-{state}','{native_key} {state}','{scope_type}','{scope_value}','{life}',{int(state=='suppressed')})")
                native_ids=json.loads(sql("BEGIN; INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,activation_suppressed) VALUES "+','.join(native_values)+f""";
                  UPDATE memories SET valid_from=(now()+interval '1 day')::text WHERE key='{native_key}-future';
                  UPDATE memories SET valid_until=(now()-interval '1 day')::text WHERE key='{native_key}-expired';
                  UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index' AND document_id IN (SELECT id FROM memories WHERE key LIKE '{native_key}-%');
                  SELECT json_object_agg(key,id) FROM memories WHERE key LIKE '{native_key}-%'; COMMIT"""))
                for audience,value,state in [('project',key,'current'),('workspace',key+'-team','workspace')]:
                    code,result,elapsed=call('recall',dict(task_hint=native_key,limit_tokens=8192,**{audience:value}))
                    rows=result.get('recall',{}).get(section,[])
                    selected={int(row['memory_id']) for row in rows if int(row['memory_id']) in set(native_ids.values())}
                    check('Common native '+section+' '+audience+' lifecycle and scope',code==200
                          and result.get('status')=='ok' and selected=={native_ids[native_key+'-'+state]},elapsed)
            history_key=key+'-history'
            history_values=[]
            for state in states:
                lifecycle=state if state in ('superseded','archived','quarantined','deleted','revoked') else 'active'
                scope=key+'-foreign' if state=='cross-scope' else key
                start='2020-03-01T00:00:00Z' if state=='future' else '2020-01-01T00:00:00Z'
                end='2020-02-01T00:00:00Z' if state=='expired' else '2020-04-01T00:00:00Z'
                history_values.append(f"('L2','fact','{history_key}-{state}','retained old {state}','project','{scope}','{lifecycle}',{int(state=='suppressed')},'{start}','{end}')")
            history_ids=json.loads(sql("BEGIN; INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,lifecycle_state,activation_suppressed,valid_from,valid_until) VALUES "+','.join(history_values)+f""";
              INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status) VALUES('{history_key}','assert','fixture','system',100,'open');
              INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,valid_from,valid_until,asserted_at,commit_id)
                SELECT '{history_key}','uses',key,'semantic','world_fact','persistent','A',.9,80,'2020-01-01T00:00:00Z','2020-04-01T00:00:00Z','2020-01-01T00:00:00Z','{history_key}' FROM memories WHERE key LIKE '{history_key}-%';
              INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance)
                SELECT e.id,'memory','memory:'||m.id::text,'supports' FROM entity_edges e JOIN memories m ON m.key=e.target WHERE e.source='{history_key}';
              INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,valid_at,invalid_at)
                SELECT id,'{history_key}','uses',key,'2020-01-01T00:00:00Z','2020-04-01T00:00:00Z' FROM memories WHERE key LIKE '{history_key}-%';
              UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index' AND document_id IN (SELECT id FROM memories WHERE key LIKE '{history_key}-%');
              SELECT json_object_agg(key,id) FROM memories WHERE key LIKE '{history_key}-%'; COMMIT"""))
            historical_wanted={history_key+'-'+state for state in ('current','superseded','archived')}
            at='2020-02-01T00:00:00Z'
            code,result,elapsed=call('search_assertions',dict(query=history_key,project=key,valid_at=at,limit=64))
            check('Historical assertions use requested-time parent eligibility',code==200 and result.get('status')=='ok'
                  and {row['object'] for row in result.get('assertions',[])}==historical_wanted,elapsed)
            code,result,elapsed=call('search_graph_as_of',dict(query=history_key,project=key,as_of=at,limit=64))
            check('Historical relations use requested-time parent eligibility',code==200 and result.get('status')=='ok'
                  and {int(row['memory_id']) for row in result.get('relations',[])}=={history_ids[name] for name in historical_wanted},elapsed)
            code,result,elapsed=call('assemble_typed_context',dict(query=history_key,project=key,valid_at=at,
                enable_historical=True,enable_observations=False,enable_approved_procedures=False,
                channel_budgets=dict(total=4096,historical_assertions=4096)))
            refs=result.get('retained_items',[])
            check('Historical typed projection retains authorized old parents',code==200 and result.get('status')=='ok'
                  and len(refs)==3 and {int(parent['record_id']) for ref in refs for parent in ref['source_version'].get('memory_parents',[])}=={history_ids[name] for name in historical_wanted},elapsed)
            code,revalidated,_=call('revalidate_sources',dict(project=key,revalidation=dict(schema_version=1,check_id=uuid.uuid4().hex,sources=refs)))
            check('Historical typed release preserves requested-time policy',code==200 and revalidated.get('eligible') is True)
            sql(f"UPDATE memories SET lifecycle_state='revoked' WHERE id={history_ids[history_key+'-superseded']}")
            code,revalidated,_=call('revalidate_sources',dict(project=key,revalidation=dict(schema_version=1,check_id=uuid.uuid4().hex,sources=refs)))
            check('Revocation after historical selection refuses release',code==200 and revalidated.get('eligible') is False)
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
