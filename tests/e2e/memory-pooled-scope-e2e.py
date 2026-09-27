#!/usr/bin/env python3
"""MR-01 actual non-owner process: concurrent scopes and failed-query rollback."""
import argparse
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', ROOT / 'tests/e2e/deployment-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    checks = []
    def check(name,passed):
        checks.append(dict(name=name,passed=bool(passed)))
        print(('PASS ' if passed else 'FAIL ')+name,flush=True)
        if not passed:
            raise RuntimeError(name)
    env = dict(os.environ,AIMEE_RUNTIME_WEB_ENABLED='0',AIMEE_POSTGRES_VOLUME_MIB='512',
               COMPOSE_PROFILES='',EMBEDDER_MODEL='bekko-a25m',
               EMBEDDER_URL='https://aimee-embedder:8762',EMBEDDER_DIMS='384',
               AIMEE_PROVIDER_CONTEXT_LIMITS=json.dumps(dict(schema_version=1,max_request_bytes=32768)))
    kb = matrix.Stack('kb',env,args.output)
    key = 'mr01-pool-'+uuid.uuid4().hex
    def sql(query):
        return matrix.command('docker','exec',kb.postgres,'psql','-U','postgres','-d','aimee_store',
                              '-X','-qAt','-v','ON_ERROR_STOP=1','-c',query)
    try:
        kb.start()
        with matrix.paused_relation_consumer(kb):
            ids = json.loads(sql(f"""BEGIN;
            INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,valid_from) VALUES
              ('L2','fact','{key}-a','{key}-a-content','project','{key}-a',''),
              ('L2','fact','{key}-b','{key}-b-content','project','{key}-b',''),
              ('L2','fact','{key}-bad','{key}-bad-content','project','{key}-a','not-a-timestamp');
            UPDATE kb_async_jobs SET status='done' WHERE kind='memory_index' AND document_id IN
              (SELECT id FROM memories WHERE key LIKE '{key}-%');
            SELECT json_object_agg(key,id::text) FROM memories WHERE key LIKE '{key}-%'; COMMIT"""))
            def sample(index):
                case = index % 6
                project = key+('-b' if case in (1,3,5) else '-a')
                target = ('a','b','b','a','bad','missing')[case]
                record_id = ids.get(key+'-'+target,'9223372036854775807')
                code,body = kb.kb_request('/v1/actions/memory.get',dict(id=record_id,scope_context=True,project=project))
                if case in (0,1):
                    return code == 200 and body.get('status') == 'ok' and body.get('memory',{}).get('content') == key+'-'+target+'-content'
                return code in (200,503) and body.get('kind') == ('unavailable' if case == 4 else 'not_found') and 'memory' not in body
            check('Each baseline scope/denial/rollback case behaves as specified',all(sample(i) for i in range(6)))
            role = json.loads(sql("""SELECT json_build_object('superuser',r.rolsuper,'bypass_rls',r.rolbypassrls,
              'owns_memories',c.relowner=r.oid,'live_connections',(SELECT count(*) FROM pg_stat_activity WHERE usename=r.rolname))
              FROM pg_roles r,pg_class c WHERE r.rolname='aimee_store_runtime' AND c.oid='memories'::regclass"""))
            check('Actual runtime has live non-owner non-superuser NOBYPASSRLS connections',
                  role['live_connections']>0 and not any(role[x] for x in ('superuser','bypass_rls','owns_memories')))
            (args.output/'runtime-role.json').write_text(json.dumps(role,indent=2)+'\n')
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                results = list(pool.map(sample,range(288)))
            check('288 concurrent mixed-scope requests isolate results after success and rollback',all(results))
            check('Sequential requests remain correct after concurrent rollback workload',all(sample(i) for i in range(6)))
        identities = []
        for name in (kb.application,kb.postgres,kb.embedder):
            value = json.loads(matrix.command('docker','inspect',name))[0]
            identities.append(dict(name=name,image=value['Image'],configured_image=value['Config']['Image']))
        (args.output/'image-identities.json').write_text(json.dumps(identities,indent=2)+'\n')
    except (RuntimeError,OSError,ValueError,subprocess.SubprocessError) as error:
        checks.append(dict(name='pooled scope HTTP completed',passed=False,error=str(error)))
    finally:
        (args.output/'checks.json').write_text(json.dumps(checks,indent=2)+'\n')
        kb.compose('down','--volumes','--remove-orphans')
    return 0 if checks and all(row['passed'] for row in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
