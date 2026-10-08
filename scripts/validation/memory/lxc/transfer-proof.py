#!/usr/bin/env python3
"""Disposable fixture migration proof; never run on a production database."""
import hashlib,json,os,subprocess,sys,time
from pathlib import Path
root=Path('/opt/aimee');out=Path('/var/lib/aimee-memory-lab/transfer-proof');out.mkdir(exist_ok=True)
env=os.environ.copy();env.update(PGDATABASE='aimee_validation')
source=Path(sys.argv[1]);schema='memory_transfer_'+str(time.time_ns());dump=subprocess.run(['runuser','-u','postgres','--','pg_dump','--schema-only','--no-owner','--no-privileges','-n','aimee_private','aimee_validation'],capture_output=True,text=True,check=True).stdout
# Known disposable schema identifier substitution on a schema-only fixture.
dump=dump.replace('aimee_private',schema)
def sql(statement):
 r=subprocess.run(['runuser','-u','postgres','--','psql','-X','-q','-A','-t','-v','ON_ERROR_STOP=1','-d','aimee_validation'],input=statement,capture_output=True,text=True)
 if r.returncode:
  (out/'fixture-execution-private.log').write_text(r.stderr);raise RuntimeError('fixture SQL failed; inspect private transfer log')
 return r.stdout.strip()
with (out/'fixture-schema-private.log').open('w') as log:
 r=subprocess.run(['runuser','-u','postgres','--','psql','-X','-q','-v','ON_ERROR_STOP=1','-d','aimee_validation'],input=dump,text=True,stdout=log,stderr=subprocess.STDOUT,check=True)
sql(f'INSERT INTO {schema}.memory_send_barrier(id) VALUES(1); INSERT INTO {schema}.user_memory_collection_generation(id,owner_id,generation) VALUES(1,gen_random_uuid(),0); INSERT INTO {schema}.user_memory_erasure_epoch(id) VALUES(1);')
validator=str(root/'src/build/obj/aimee-memory-transfer');native=root/'scripts/validation/memory/native-transfer.py'
# Round trip through two genuinely separate adapter catalogs, preserving tokens.
namespace='11111111-1111-4111-8111-111111111111';current=source
checks=[]
for engine in ['cognee','hillock']:
 catalog=out/(engine+'-'+schema);result=out/(engine+'-'+schema+'.json')
 for operation,file in [('import',current),('export',result)]:
  subprocess.run([validator,'-operation',operation,'-directory',str(catalog),'-namespace',namespace,'-file',str(file)],check=True,stdout=subprocess.DEVNULL)
 incoming=json.loads(current.read_bytes());outgoing=json.loads(result.read_bytes())
 assert incoming['entries']==outgoing['entries']
 assert incoming['owner']==outgoing['owner']
 checks.append({'name':engine+' independent catalog retains exact IDs/revisions/history/authorship/metadata','status':'passed'})
 current=result
current.chmod(0o644) # Synthetic fixture only, readable by PostgreSQL OS account.
command=['runuser','-u','postgres','--','env','PGDATABASE=aimee_validation','python3',str(native),'native-import','--placement','server','--schema',schema,'--file',str(current),'--validator',validator]
# PostgreSQL OS user cannot traverse the catalog's private parent; make an
# explicit disposable fixture copy outside that catalog before invoking it.
fixture=Path('/tmp/'+schema+'.json');fixture.write_bytes(current.read_bytes());fixture.chmod(0o644);command[command.index(str(current))]=str(fixture)
r=subprocess.run(command,capture_output=True,text=True)
(out/'native-import.log').write_text(r.stdout+r.stderr)
if r.returncode:raise RuntimeError('native import failed; inspect transfer-proof/native-import.log')
snapshot=json.loads(current.read_bytes())
for entry in snapshot['entries'].values():
 record=entry['record'];id=int(record['id']);value=json.loads(sql(f"SELECT json_build_object('id',id,'content',content,'revision',record_revision,'author',author_principal) FROM {schema}.user_memories WHERE id={id}"))
 assert value['content']==record['content'] and str(value['revision'])==record['version']['record_revision'] and value['author']==record.get('authorship',{}).get('principal','')
 for history in entry['history']:
  version=int(history['version']['record_revision']);body=sql(f'SELECT record->>\'content\' FROM {schema}.user_memory_versions WHERE memory_id={id} AND record_revision={version}')
  assert body==history['content']
checks.append({'name':'native import preserves exact canonical content, IDs, revisions and retained history','status':'passed'})
# A second import into the occupied target must refuse without changing rows.
r=subprocess.run(command,capture_output=True,text=True);assert r.returncode!=0
checks.append({'name':'occupied native target refuses replacement','status':'passed'})
# The imported portable guard must retain erasure restrictions.
assert sql(f"SELECT count(*) FROM {schema}.memory_backend_transfer_control WHERE state->'erased' IS NOT NULL")=='1'
checks.append({'name':'native target retains portable erasure controls','status':'passed'})
# Exercise retained controls, rather than merely checking the control table.
record_id=int(next(iter(snapshot['entries'].values()))['record']['id'])
sql(f"UPDATE {schema}.memory_backend_transfer_control SET state=jsonb_set(state,'{{erased}}',coalesce(state->'erased','{{}}'::jsonb)||jsonb_build_object('{record_id}',true));")
r=subprocess.run(['runuser','-u','postgres','--','psql','-X','-q','-v','ON_ERROR_STOP=1','-d','aimee_validation'],input=f"UPDATE {schema}.user_memories SET content=content WHERE id={record_id};",text=True,capture_output=True)
assert r.returncode!=0 and 'portable erasure restoration prohibited' in r.stderr
checks.append({'name':'native target refuses restoration of portable erased ID','status':'passed'})

(out/'summary.json').write_text(json.dumps({'checks':checks,'status':'passed','source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'records':len(snapshot['entries']),'retained_history':sum(len(e['history']) for e in snapshot['entries'].values())},indent=2))
fixture.unlink();sql(f'DROP SCHEMA {schema} CASCADE;')
print(json.dumps({'status':'passed','checks':checks}))
