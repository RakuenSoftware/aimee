-- Run by the PostgreSQL administrator before restricted application migrations.
-- No login, canonical access, schema creation, or role administration is granted.
DO $hygiene_role$
BEGIN
 IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_memory_hygiene') THEN
  CREATE ROLE aimee_memory_hygiene NOLOGIN NOINHERIT NOBYPASSRLS;
 END IF;
 IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_memory_hygiene' AND
  (rolsuper OR rolbypassrls OR rolcreaterole OR rolcreatedb OR rolcanlogin OR rolreplication)) THEN
  RAISE EXCEPTION 'unsafe hygiene worker role';
 END IF;
 IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
  GRANT aimee_memory_hygiene TO aimee_store_runtime WITH INHERIT FALSE;
 END IF;
END $hygiene_role$;
