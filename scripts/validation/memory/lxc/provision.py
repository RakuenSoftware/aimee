#!/usr/bin/env python3
import json, os, secrets, subprocess
from pathlib import Path
root=Path('/opt/aimee')
env_path=Path('/root/aimee-validation-env.json')
if env_path.exists(): raise SystemExit('refusing to overwrite existing validation credentials')
owner, runtime=secrets.token_hex(24), secrets.token_hex(24)
def sql(text, database='postgres'):
 return subprocess.run(['runuser','-u','postgres','--','/usr/lib/postgresql/18/bin/psql','-h','/var/run/postgresql','-X','-v','ON_ERROR_STOP=1','-d',database],input=text,text=True,check=True)
sql("DO $$ BEGIN IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='ct_owner') THEN CREATE ROLE ct_owner LOGIN; END IF; IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='ct_runtime') THEN CREATE ROLE ct_runtime LOGIN; END IF; END $$;")
sql(f"ALTER ROLE ct_owner LOGIN PASSWORD '{owner}'; ALTER ROLE ct_runtime LOGIN PASSWORD '{runtime}' NOBYPASSRLS NOSUPERUSER NOCREATEDB NOCREATEROLE;")
sql("DROP DATABASE IF EXISTS aimee_validation WITH (FORCE);")
sql("CREATE DATABASE aimee_validation OWNER ct_owner TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE 'C.UTF-8' LC_CTYPE 'C.UTF-8';")
sql("CREATE EXTENSION vector; CREATE EXTENSION pg_trgm; CREATE SCHEMA aimee_private AUTHORIZATION ct_owner; GRANT USAGE ON SCHEMA aimee_private,public TO ct_runtime;",'aimee_validation')
sql((root/'src/modules/kb/c/schema_roles.sql').read_text(),'aimee_validation')
sql((root/'scripts/postgres-hygiene-role.sql').read_text(),'aimee_validation')
sql('GRANT aimee_kb_owner TO ct_owner WITH INHERIT TRUE;','aimee_validation')
sql('SET ROLE ct_owner;'+(root/'src/modules/kb/c/schema.sql').read_text().replace('__EMBED_DIM__','1024'),'aimee_validation')
sql((root/'src/modules/kb/c/schema_grants.sql').read_text(),'aimee_validation')
sql('GRANT aimee_kb_runtime TO ct_runtime WITH INHERIT TRUE; GRANT aimee_memory_hygiene TO ct_runtime WITH INHERIT FALSE; GRANT USAGE ON SCHEMA public TO ct_runtime; ALTER DEFAULT PRIVILEGES FOR ROLE ct_owner IN SCHEMA aimee_private GRANT SELECT,INSERT,UPDATE,DELETE ON TABLES TO ct_runtime; ALTER DEFAULT PRIVILEGES FOR ROLE ct_owner IN SCHEMA aimee_private GRANT USAGE,SELECT ON SEQUENCES TO ct_runtime;','aimee_validation')
def url(password,user,schema):return f'postgresql://{user}:{password}@127.0.0.1:5432/aimee_validation?search_path={schema}'
env={'AIMEE_STORE_URL':url(runtime,'ct_runtime','aimee_private'),'AIMEE_STORE_MIGRATION_URL':url(owner,'ct_owner','aimee_private'),'AIMEE_CT_KB_STORE_URL':url(runtime,'ct_runtime','public'),'AIMEE_CT_KB_MIGRATION_URL':url(owner,'ct_owner','public')}
env_path.write_text(json.dumps(env));env_path.chmod(0o600)
print('Distinct owner/runtime credentials and private/shared schemas provisioned.')
