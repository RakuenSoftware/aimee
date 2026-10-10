#!/usr/bin/env python3
"""Offline native record <-> external catalog bridge; see integrations/memory/CONTRACT.md.

Uses an operator's normal PG* environment and psql (never a DSN argument).
Stop the target memory owner before importing. No provider graph/index is copied.
"""
import argparse,hashlib,json,os,re,subprocess,sys,uuid,tempfile
from pathlib import Path
MAX=64<<20

def sql(statement):
 r=subprocess.run(['psql','-X','-q','-A','-t','-v','ON_ERROR_STOP=1'],input=statement,text=True,capture_output=True)
 if r.returncode:raise RuntimeError('native transfer SQL refused; inspect private database logs')
 return r.stdout.strip()

def literal(value):return "'"+str(value).replace("'","''")+"'"
def marker(value):return 'sha256:'+hashlib.sha256(value.encode()).hexdigest()
def record(raw,owner,private):
 scope={'type':'user','value':'_user'} if private else {'type':raw['scope_type'],'value':raw['scope_value']}
 author=raw.get('_portable_authorship') or {'category':raw.get('provenance_category','unknown'),'principal':raw.get('author_principal',raw.get('owner_principal','')),'transport':raw.get('author_transport','internal'),'session_id':raw.get('source_session','')}
 result={k:raw[k] for k in ['id','tier','kind','key','content','confidence']}
 result.update(scope=scope,version={'schema_version':1,'owner_id':owner,'record_id':str(raw['id']),'record_revision':str(raw['record_revision'])},authorship=author,metadata={'aimee_native_row':raw})
 for name in ['valid_from','valid_until']:
  if raw.get(name):
   value=raw[name].replace(' ','T')
   if '+' not in value and not value.endswith('Z'):value+='Z'
   result[name]=value
 if raw.get('sensitivity'):result['sensitivity']=raw['sensitivity']
 return result

def export(args):
 private=args.placement=='server';table='user_memories' if private else 'memories';owner='user_memory_collection_generation' if private else 'memory_collection_owner';prefix='user_memory' if private else 'memory'
 statements=f"SET search_path={args.schema},pg_catalog; BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY; SELECT json_build_object('owner',(SELECT owner_id::text FROM {owner} WHERE id=1),'records',COALESCE((SELECT json_agg(to_jsonb(m) ORDER BY id) FROM {table} m),'[]'::json),'intents',COALESCE((SELECT json_agg(to_jsonb(i)) FROM {prefix}_erasure_intents i),'[]'::json),'sessions',COALESCE((SELECT json_agg(session_digest) FROM {prefix}_erasure_sessions),'[]'::json)"
 if private:statements+=",'history',COALESCE((SELECT json_agg(to_jsonb(v) ORDER BY memory_id,record_revision) FROM user_memory_versions v),'[]'::json)"
 else:statements+=",'actors',COALESCE((SELECT json_agg(to_jsonb(a)) FROM memory_fact_actors a),'[]'::json)"
 statements+="); COMMIT;"
 data=json.loads(sql(statements));snapshot={'version':1,'owner':data['owner'],'next':1,'entries':{},'erased':{},'receipts':{},'erased_subjects':{},'erased_payloads':{}}
 control={}
 if sql(f"SELECT to_regclass({literal(args.schema+'.memory_backend_transfer_control')})"):
  control=json.loads(sql(f'SELECT state FROM {args.schema}.memory_backend_transfer_control WHERE id=1'))
 for name in ['erased','erased_subjects','erased_payloads','subject_epochs','erasure_requests']:
  snapshot[name].update(control.get(name,{})) if name in snapshot else snapshot.update({name:control.get(name,{})})
 histories={}
 for h in data.get('history',[]):histories.setdefault(h['memory_id'],[]).append(record(h['record'],data['owner'],private))
 actors={a['memory_id']:a for a in data.get('actors',[])}
 for raw in data['records']:
  if raw['id'] in actors:
   a=actors[raw['id']];raw['_portable_authorship']={'category':'user_stated' if a['actor_role']=='user' else 'agent_message','principal':a['actor_principal'],'transport':a.get('transport_identity','internal'),'session_id':raw.get('source_session','')}
  r=record(raw,data['owner'],private)
  principal=r['authorship'].get('principal','')
  if snapshot.get('subject_epochs',{}).get(marker(principal)):
   r['authorship']['erasure_epoch']=str(snapshot['subject_epochs'][marker(principal)])
  snapshot['entries'][str(r['id'])]={'record':r,'retired':raw['lifecycle_state']!='active','history':histories.get(r['id'],[])}
  snapshot['next']=max(snapshot['next'],r['id']+1)
 for intent in data['intents']:
  snapshot['erased'][str(intent['memory_id'])]=True;snapshot['next']=max(snapshot['next'],intent['memory_id']+1)
  audience='*' if private else intent['scope_type']+'/'+intent['scope_value']
  snapshot['erased_payloads'][audience+':'+intent['payload_digest']]=True
 for digest in data['sessions']:snapshot['erased_subjects']['sha256:'+digest]=True
 raw=json.dumps(snapshot,ensure_ascii=False).encode()
 if len(raw)>MAX:raise RuntimeError('snapshot exceeds transfer capacity')
 fd=os.open(args.file,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
 with os.fdopen(fd,'wb') as f:f.write(raw);f.flush();os.fsync(f.fileno())
 print(json.dumps({'operation':'native-export','records':len(snapshot['entries']),'history':sum(len(e['history']) for e in snapshot['entries'].values()),'sha256':hashlib.sha256(raw).hexdigest()}))

def import_native(args):
 # The generic catalog validates identity, revisions, history and erasure state.
 with tempfile.TemporaryDirectory(prefix='aimee-memory-transfer-') as directory:
  r=subprocess.run([args.validator,'-operation','import','-directory',directory,'-namespace','11111111-1111-4111-8111-111111111111','-file',args.file],capture_output=True,text=True)
  if r.returncode:raise RuntimeError('portable catalog validation failed')
 data=json.loads(Path(args.file).read_bytes())
 if len(Path(args.file).read_bytes())>MAX or data.get('version')!=1:raise RuntimeError('invalid bounded snapshot')
 uuid.UUID(data['owner']);private=args.placement=='server';table='user_memories' if private else 'memories';owner='user_memory_collection_generation' if private else 'memory_collection_owner'
 allowed=sql(f"SELECT attname FROM pg_attribute WHERE attrelid={literal(args.schema+'.'+table)}::regclass AND attnum>0 AND NOT attisdropped AND attgenerated='' ORDER BY attnum").splitlines()
 statements=[f"BEGIN; SET LOCAL search_path={args.schema},pg_catalog; LOCK TABLE {table} IN ACCESS EXCLUSIVE MODE; DO $$BEGIN IF EXISTS(SELECT 1 FROM {table}) THEN RAISE EXCEPTION 'target must be empty';END IF;END$$;",f"UPDATE {owner} SET owner_id={literal(data['owner'])}::uuid WHERE id=1;"]
 control={k:data.get(k,{}) for k in ['erased','erased_subjects','erased_payloads','subject_epochs','erasure_requests']}
 control_json=literal(json.dumps(control))
 statements += [f'''CREATE TABLE IF NOT EXISTS memory_backend_transfer_control(id INTEGER PRIMARY KEY CHECK(id=1),state JSONB NOT NULL);
 REVOKE ALL ON memory_backend_transfer_control FROM PUBLIC;
 DO $acl$DECLARE recipient RECORD;BEGIN FOR recipient IN SELECT DISTINCT grantee FROM pg_class c,LATERAL aclexplode(c.relacl) a WHERE c.oid='memory_backend_transfer_control'::regclass AND a.grantee<>c.relowner AND a.grantee<>0 LOOP EXECUTE format('REVOKE ALL ON memory_backend_transfer_control FROM %I',pg_get_userbyid(recipient.grantee));END LOOP;END$acl$;
 INSERT INTO memory_backend_transfer_control VALUES(1,'{{}}') ON CONFLICT DO NOTHING;
 LOCK TABLE memory_backend_transfer_control IN ACCESS EXCLUSIVE MODE;
 UPDATE memory_backend_transfer_control SET state=jsonb_build_object(
 'erased',COALESCE(state->'erased','{{}}')||({control_json}::jsonb->'erased'),
 'erased_subjects',COALESCE(state->'erased_subjects','{{}}')||({control_json}::jsonb->'erased_subjects'),
 'erased_payloads',COALESCE(state->'erased_payloads','{{}}')||({control_json}::jsonb->'erased_payloads'),
 'erasure_requests',COALESCE(state->'erasure_requests','{{}}')||({control_json}::jsonb->'erasure_requests'),
 'subject_epochs',COALESCE((SELECT jsonb_object_agg(key,value) FROM (SELECT key,max(value::bigint) AS value FROM (SELECT * FROM jsonb_each_text(COALESCE(state->'subject_epochs','{{}}')) UNION ALL SELECT * FROM jsonb_each_text({control_json}::jsonb->'subject_epochs')) all_epochs GROUP BY key) epochs),'{{}}'::jsonb)) WHERE id=1;''']
 # Keep every erasure/serving-policy trigger enabled. Disable only mechanical
 # reassignment/capture of authorship and revision while importing exact heads.
 triggers=['user_memory_00_authority','user_memory_assign_revision'] if private else ['memory_assign_record_revision']
 for trigger in triggers:statements.append(f'ALTER TABLE {table} DISABLE TRIGGER {trigger};')
 statements.append('''CREATE OR REPLACE FUNCTION memory_backend_transfer_guard() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog AS $$
 DECLARE state JSONB; r JSONB; audience TEXT; digest TEXT; principal TEXT; expected TEXT;
 BEGIN
 EXECUTE format('SELECT state FROM %I.memory_backend_transfer_control WHERE id=1 FOR SHARE',TG_TABLE_SCHEMA) INTO state;
 r:=CASE WHEN TG_TABLE_NAME='user_memory_versions' THEN to_jsonb(NEW)->'record' ELSE to_jsonb(NEW) END;
 audience:=CASE WHEN TG_TABLE_NAME IN ('user_memories','user_memory_versions') THEN '*' ELSE (r->>'scope_type')||'/'||(r->>'scope_value') END;
 digest:=encode(sha256(convert_to(r->>'content','UTF8')),'hex');
 IF COALESCE(current_setting('aimee.memory_transfer_admission',true),'')<>'' THEN
 principal:=COALESCE(r->>'author_principal',r->>'owner_principal','');
 expected:=state->'subject_epochs'->>('sha256:'||encode(sha256(convert_to(principal,'UTF8')),'hex'));
 IF expected IS NOT NULL AND expected IS DISTINCT FROM (current_setting('aimee.memory_transfer_admission',true)::jsonb->>((r->>'id')||':'||(r->>'record_revision'))) THEN RAISE EXCEPTION 'portable stale author admission' USING ERRCODE='23514';END IF;
 END IF;
 IF COALESCE((state->'erased'->>(r->>'id'))::boolean,false)
 OR COALESCE((state->'erased_payloads'->>('*:'||digest))::boolean,false)
 OR COALESCE((state->'erased_payloads'->>(audience||':'||digest))::boolean,false)
 OR COALESCE((state->'erased_subjects'->>('sha256:'||encode(sha256(convert_to(COALESCE(r->>'source_session',''),'UTF8')),'hex')))::boolean,false)
 THEN RAISE EXCEPTION 'portable erasure restoration prohibited' USING ERRCODE='23514';END IF;
 RETURN NEW;
 END$$;
 REVOKE ALL ON FUNCTION memory_backend_transfer_guard() FROM PUBLIC;''')
 for relation in ([table,'user_memory_versions'] if private else [table]):
  statements += [f'DROP TRIGGER IF EXISTS memory_backend_transfer_guard ON {relation};',f'CREATE TRIGGER memory_backend_transfer_guard BEFORE INSERT OR UPDATE ON {relation} FOR EACH ROW EXECUTE FUNCTION memory_backend_transfer_guard();']
 entries=data.get('entries',{})
 admissions={}
 for entry in entries.values():
  for item in [entry['record']]+entry.get('history',[]):admissions[str(item['id'])+':'+item['version']['record_revision']]=item.get('authorship',{}).get('erasure_epoch','0')
 statements.append("SELECT set_config('aimee.memory_transfer_admission',"+literal(json.dumps(admissions))+",true);")
 for entry in entries.values():
  r=entry['record'];v=r['version'];uuid.UUID(v['owner_id'])
  if v['owner_id']!=data['owner'] or v['record_id']!=str(r['id']) or int(v['record_revision'])<=0 or str(r['id']) in data.get('erased',{}):raise RuntimeError('inconsistent identity or erased record')
  scope=r['scope']
  if private and scope!={'type':'user','value':'_user'}:raise RuntimeError('wrong placement')
  if not private and scope['type']=='user':raise RuntimeError('wrong placement')
  author=r.get('authorship',{});epoch=str(data.get('subject_epochs',{}).get(marker(author.get('principal','')),0))
  if data.get('erased_subjects',{}).get(marker(author.get('principal',''))) and (epoch=='0' or author.get('erasure_epoch')!=epoch):raise RuntimeError('erased authorship')
  if data.get('erased_subjects',{}).get(marker(author.get('session_id',''))):raise RuntimeError('erased session')
  digest=hashlib.sha256(r['content'].encode()).hexdigest()
  if data.get('erased_payloads',{}).get('*:'+digest) or data.get('erased_payloads',{}).get(scope['type']+'/'+scope['value']+':'+digest):raise RuntimeError('erased payload')
  raw=r.get('metadata',{}).get('aimee_native_row',{}).copy()
  raw.update({k:r[k] for k in ['id','tier','kind','key','content','confidence']});raw['record_revision']=int(v['record_revision']);raw['lifecycle_state']='retired' if entry['retired'] else 'active';raw['source_session']=author.get('session_id','')
  if private:raw.update(provenance_category=author.get('category','unknown'),author_principal=author.get('principal',''),author_transport=author.get('transport',''))
  else:raw.update(scope_type=scope['type'],scope_value=scope['value'],owner_principal=author.get('principal',''))
  raw.update({k:r[k] for k in ['valid_from','valid_until','sensitivity'] if k in r})
  columns=[c for c in allowed if c in raw]
  names=','.join('"'+c+'"' for c in columns)
  statements.append(f"INSERT INTO {table}({names}) SELECT {names} FROM json_populate_record(NULL::{table},{literal(json.dumps(raw))}::json);")
  if private:
   for h in entry.get('history',[]):
    hr=h.get('metadata',{}).get('aimee_native_row',{}).copy();hr.update({k:h[k] for k in ['id','tier','kind','key','content','confidence']});hr['record_revision']=int(h['version']['record_revision']);hr['lifecycle_state']='active';ha=h.get('authorship',{});hr.update(provenance_category=ha.get('category','unknown'),author_principal=ha.get('principal',''),author_transport=ha.get('transport',''),source_session=ha.get('session_id',''))
    statements.append(f"INSERT INTO user_memory_versions(memory_id,record_revision,record) VALUES({r['id']},{hr['record_revision']},{literal(json.dumps(hr))}::jsonb);")
  elif author.get('principal'):
   statements.append(f"INSERT INTO memory_fact_actors(memory_id,actor_principal,actor_role,authority_rank,authenticated,transport_identity) VALUES({r['id']},{literal(author['principal'])},{literal('user' if author.get('category')=='user_stated' else 'model')},0,1,{literal(author.get('transport',''))});")
 for trigger in triggers:statements.append(f'ALTER TABLE {table} ENABLE TRIGGER {trigger};')
 statements.append(f"SELECT setval(pg_get_serial_sequence({literal(args.schema+'.'+table)},'id'),{max(1,int(data['next']))},false); COMMIT;")
 sql('\n'.join(statements))
 print(json.dumps({'operation':'native-import','records':len(entries)}))

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('operation',choices=['native-export','native-import']);p.add_argument('--placement',choices=['server','kb'],required=True);p.add_argument('--schema',required=True);p.add_argument('--file',required=True);p.add_argument('--validator',default='aimee-memory-transfer');a=p.parse_args()
 if not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*',a.schema):raise RuntimeError('invalid schema')
 if a.operation=='native-export':export(a)
 else:import_native(a)
if __name__=='__main__':
 try:main()
 except Exception as error:
  # Never echo payloads, SQL, credentials or subprocess stderr.
  print('Native transfer refused: '+(str(error) if isinstance(error,RuntimeError) else type(error).__name__),file=sys.stderr);raise SystemExit(1)
