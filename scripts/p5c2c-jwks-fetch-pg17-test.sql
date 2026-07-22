\set ON_ERROR_STOP on
BEGIN;

DO $$
DECLARE f REGPROCEDURE := 'public.kb_management_jwks_runtime_fetch(text,text,text)'::regprocedure;
 owner_oid OID;
BEGIN
 SELECT oid INTO STRICT owner_oid FROM pg_roles WHERE rolname='aimee_kb_jwks_runtime_definer';
 IF NOT EXISTS(SELECT 1 FROM pg_proc WHERE oid=f AND prosecdef AND proowner=owner_oid
   AND provolatile='v' AND proconfig @> ARRAY['search_path=pg_catalog, pg_temp']::TEXT[]) THEN
  RAISE EXCEPTION 'P5-C2c runtime facade hardening mismatch';
 END IF;
 IF NOT has_function_privilege('aimee_kb_runtime',f,'EXECUTE') OR
    has_function_privilege('aimee_kb_jwks_publish',f,'EXECUTE') OR
    has_function_privilege('aimee_kb_status',f,'EXECUTE') OR
    has_function_privilege('public',f,'EXECUTE') THEN
  RAISE EXCEPTION 'P5-C2c runtime facade ACL mismatch';
 END IF;
 IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_jwks_runtime_definer'
    AND NOT rolcanlogin AND NOT rolinherit AND rolbypassrls AND NOT rolsuper
    AND NOT rolcreatedb AND NOT rolcreaterole AND NOT rolreplication) OR
    pg_has_role('aimee_kb_runtime','aimee_kb_jwks_runtime_definer','MEMBER') THEN
  RAISE EXCEPTION 'P5-C2c runtime definer role hardening mismatch';
 END IF;
 IF (SELECT count(*) FROM pg_proc WHERE proowner=owner_oid)<>1 OR
    EXISTS(SELECT 1 FROM pg_class WHERE relowner=owner_oid) OR
    (SELECT proowner=owner_oid FROM pg_proc
      WHERE oid='public.org_vault_control_require_open()'::regprocedure) THEN
  RAISE EXCEPTION 'P5-C2c runtime definer ownership closure mismatch';
 END IF;
 IF NOT has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_server_registry','SELECT') OR
    NOT has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_enrollments','SELECT') OR
    NOT has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_management_jwks_publication_candidate','SELECT') OR
    NOT has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_management_jwks_publication_generation','SELECT') OR
    NOT has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_management_jwks_publication_registry','SELECT') OR
    has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_management_jwks_publication_permit','SELECT') OR
    has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_management_jwks_publication_permit','INSERT') OR
    has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_management_jwks_publication_permit','UPDATE') OR
    has_table_privilege('aimee_kb_jwks_runtime_definer',
      'public.kb_management_jwks_publication_permit','DELETE') OR
    NOT has_function_privilege('aimee_kb_jwks_runtime_definer',
      'public.org_vault_control_require_open()','EXECUTE') THEN
  RAISE EXCEPTION 'P5-C2c runtime definer privilege closure mismatch';
 END IF;
 IF has_table_privilege('aimee_kb_runtime','public.kb_management_jwks_publication_generation','SELECT')
    OR has_function_privilege('aimee_kb_runtime',
      'public.kb_management_jwks_publication_final()','EXECUTE') THEN
  RAISE EXCEPTION 'P5-C2c runtime publication closure widened';
 END IF;
END $$;

INSERT INTO kb_team(id,name,operator_id) VALUES(9752201,'p5c2c-fetch-team','p5c2c-test');
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
 client_issuer,client_serial_norm,client_fingerprint)
VALUES('p5c2c-server','p5c2c-client','p5c2c-mgmt',9752201,'https://p5c2c.invalid','active',
 'p5c2c-issuer','01',repeat('a',64));
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,expires_at,revoked_at,legacy,
 cert_issuer,cert_serial_norm,authority_id)
VALUES('p5-server-client',repeat('a',64),'01','active','2999-01-01 00:00:00+00','',0,
 'p5c2c-issuer','01',repeat('a',32));
UPDATE kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id=''
 WHERE singleton=1;

SET LOCAL ROLE aimee_kb_runtime;
DO $$ BEGIN
 IF EXISTS(SELECT 1 FROM public.kb_management_jwks_runtime_fetch(
   'p5c2c-issuer','01',repeat('a',64))) THEN
  RAISE EXCEPTION 'P5-C2c leaked a non-FINAL publication';
 END IF;
 BEGIN
  PERFORM * FROM public.kb_management_jwks_runtime_fetch('p5c2c-other','01',repeat('a',64));
  RAISE EXCEPTION 'P5-C2c accepted an issuer mismatch';
 EXCEPTION WHEN SQLSTATE '28000' THEN NULL;
 END;
 BEGIN
  PERFORM * FROM public.kb_management_jwks_runtime_fetch('p5c2c-issuer','01',repeat('b',64));
  RAISE EXCEPTION 'P5-C2c accepted a fingerprint mismatch';
 EXCEPTION WHEN SQLSTATE '28000' THEN NULL;
 END;
 BEGIN
  PERFORM * FROM public.kb_management_jwks_runtime_fetch('p5c2c-issuer','02',repeat('a',64));
  RAISE EXCEPTION 'P5-C2c accepted a serial mismatch';
 EXCEPTION WHEN SQLSTATE '28000' THEN NULL;
 END;
END $$;
RESET ROLE;

UPDATE kb_server_registry SET status='pending' WHERE server_id='p5c2c-server';
DO $$ BEGIN
 BEGIN
  PERFORM * FROM kb_management_jwks_runtime_fetch('p5c2c-issuer','01',repeat('a',64));
  RAISE EXCEPTION 'P5-C2c accepted a pending registry row';
 EXCEPTION WHEN SQLSTATE '28000' THEN NULL;
 END;
END $$;
UPDATE kb_server_registry SET status='active' WHERE server_id='p5c2c-server';

UPDATE kb_enrollments SET scope='global' WHERE fingerprint=repeat('a',64);
DO $$ BEGIN
 BEGIN
  PERFORM * FROM kb_management_jwks_runtime_fetch('p5c2c-issuer','01',repeat('a',64));
  RAISE EXCEPTION 'P5-C2c accepted a wrong-purpose enrollment';
 EXCEPTION WHEN SQLSTATE '28000' THEN NULL;
 END;
END $$;
UPDATE kb_enrollments SET scope='p5-server-client' WHERE fingerprint=repeat('a',64);

UPDATE kb_enrollments SET expires_at='2000-01-01 00:00:00+00' WHERE fingerprint=repeat('a',64);
DO $$ BEGIN
 BEGIN
  PERFORM * FROM kb_management_jwks_runtime_fetch('p5c2c-issuer','01',repeat('a',64));
  RAISE EXCEPTION 'P5-C2c accepted an expired enrollment';
 EXCEPTION WHEN SQLSTATE '28000' THEN NULL;
 END;
END $$;
UPDATE kb_enrollments SET expires_at='2999-01-01 00:00:00+00'
 WHERE fingerprint=repeat('a',64);

UPDATE kb_vault_control SET sealed=true,maintenance_kind='reseal',maintenance_id='p5c2c-test'
 WHERE singleton=1;
DO $$ BEGIN
 BEGIN
  PERFORM * FROM kb_management_jwks_runtime_fetch('p5c2c-issuer','01',repeat('a',64));
  RAISE EXCEPTION 'P5-C2c authorized through sealed vault';
 EXCEPTION WHEN object_not_in_prerequisite_state THEN
  IF SQLERRM<>'org_vault_control: sealed' THEN RAISE; END IF;
 END;
END $$;
UPDATE kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id='' WHERE singleton=1;

UPDATE kb_enrollments SET revoked_at='2026-07-22',state='revoked'
 WHERE scope='p5-server-client' AND fingerprint=repeat('a',64);
DO $$ BEGIN
 BEGIN
  PERFORM * FROM kb_management_jwks_runtime_fetch('p5c2c-issuer','01',repeat('a',64));
  RAISE EXCEPTION 'P5-C2c accepted a revoked enrollment';
 EXCEPTION WHEN SQLSTATE '28000' THEN NULL;
 END;
END $$;

ROLLBACK;
\echo 'P5-C2c JWKS fetch PostgreSQL 17 gate passed'
