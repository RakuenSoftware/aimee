\set ON_ERROR_STOP on

-- Run after schema.sql and schema_grants.sql in a disposable PostgreSQL 17 DB.
BEGIN;

DO $test$
DECLARE
  status RECORD;
  capability_count BIGINT;
  object_name TEXT;
BEGIN
  IF NOT pg_catalog.has_function_privilege(
      'aimee_kb_vault_orchestrator',
      'aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()','EXECUTE') OR
     pg_catalog.has_function_privilege(
      'aimee_kb_vault_orchestrator_login',
      'aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()','EXECUTE') OR
     pg_catalog.has_function_privilege(
      'aimee_kb_runtime',
      'aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()','EXECUTE') THEN
    RAISE EXCEPTION 'P7 D3a function ACL mismatch';
  END IF;
  SELECT count(*) INTO capability_count
    FROM pg_catalog.pg_proc p
    JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
   WHERE pg_catalog.left(n.nspname,3)<>'pg_'
     AND n.nspname<>'information_schema'
     AND pg_catalog.has_schema_privilege(
       'aimee_kb_vault_orchestrator',n.oid,'USAGE')
     AND pg_catalog.has_function_privilege(
       'aimee_kb_vault_orchestrator',p.oid,'EXECUTE');
  IF capability_count<>1 THEN
    RAISE EXCEPTION 'P7 D3a capability has % effectively invocable application functions',
      capability_count;
  END IF;
  FOREACH object_name IN ARRAY ARRAY['kb_vault_control','kb_vault_rewrap_operation',
      'kb_vault_rewrap_dek_stage','kb_vault_rewrap_check_stage','kb_vault_rewrap_worm'] LOOP
    IF pg_catalog.has_table_privilege('aimee_kb_vault_orchestrator',
         'public.'||object_name,'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') OR
       pg_catalog.has_table_privilege('aimee_kb_vault_orchestrator_login',
         'public.'||object_name,'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') OR
       pg_catalog.has_table_privilege('aimee_kb_runtime','public.'||object_name,
         'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') THEN
      RAISE EXCEPTION 'P7 D3a table ACL mismatch on %',object_name;
    END IF;
  END LOOP;
  IF pg_catalog.has_schema_privilege('aimee_kb_vault_orchestrator_login',
       'aimee_kb_vault_orchestrator_api','USAGE') OR
     pg_catalog.has_schema_privilege('aimee_kb_runtime',
       'aimee_kb_vault_orchestrator_api','USAGE') OR
     EXISTS (SELECT 1 FROM pg_catalog.pg_namespace n
       CROSS JOIN LATERAL pg_catalog.aclexplode(COALESCE(
         n.nspacl,pg_catalog.acldefault('n',n.nspowner))) acl
      WHERE n.nspname='aimee_kb_vault_orchestrator_api' AND acl.grantee=0) OR
     NOT pg_catalog.has_schema_privilege('aimee_kb_vault_orchestrator',
       'aimee_kb_vault_orchestrator_api','USAGE') OR
     pg_catalog.has_schema_privilege('aimee_kb_vault_orchestrator',
       'aimee_kb_vault_orchestrator_api','CREATE') OR
     pg_catalog.has_schema_privilege('aimee_kb_vault_orchestrator','public','USAGE') OR
     pg_catalog.has_database_privilege('aimee_kb_vault_orchestrator_login',
       current_database(),'CREATE') OR
     pg_catalog.has_database_privilege('aimee_kb_vault_orchestrator_login',
       current_database(),'TEMPORARY') OR
     pg_catalog.has_database_privilege('aimee_kb_vault_orchestrator',
       current_database(),'CREATE') OR
     pg_catalog.has_database_privilege('aimee_kb_vault_orchestrator',
       current_database(),'TEMPORARY') OR
     (SELECT owner.rolname FROM pg_catalog.pg_database d
       JOIN pg_catalog.pg_roles owner ON owner.oid=d.datdba
      WHERE d.datname=current_database()) IN
       ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login') THEN
    RAISE EXCEPTION 'P7 D3a login/schema ACL mismatch';
  END IF;
  IF EXISTS (
       SELECT 1 FROM pg_catalog.pg_namespace n
       JOIN pg_catalog.pg_roles owner ON owner.oid=n.nspowner
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND owner.rolname IN
            ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_class c
       JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
       JOIN pg_catalog.pg_roles owner ON owner.oid=c.relowner
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND owner.rolname IN
            ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_proc p
       JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
       JOIN pg_catalog.pg_roles owner ON owner.oid=p.proowner
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND owner.rolname IN
            ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_class c
       JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
       CROSS JOIN LATERAL pg_catalog.aclexplode(c.relacl) acl
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND acl.grantee=(SELECT oid FROM pg_catalog.pg_roles
                            WHERE rolname='aimee_kb_vault_orchestrator'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_class c
       JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND CASE WHEN c.relkind IN ('r','p','v','m','f') THEN
            pg_catalog.has_table_privilege('aimee_kb_vault_orchestrator',c.oid,
              'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER')
            ELSE false END)
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_class c
       JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND CASE WHEN c.relkind='S' THEN
            pg_catalog.has_sequence_privilege('aimee_kb_vault_orchestrator',c.oid,
              'USAGE,SELECT,UPDATE') ELSE false END)
     OR (SELECT count(*) FROM pg_catalog.pg_proc p
       JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
       CROSS JOIN LATERAL pg_catalog.aclexplode(p.proacl) acl
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND acl.grantee=(SELECT oid FROM pg_catalog.pg_roles
                            WHERE rolname='aimee_kb_vault_orchestrator'))<>1 THEN
    RAISE EXCEPTION 'P7 D3a capability ownership/direct ACL closure mismatch';
  END IF;
  IF EXISTS (
       SELECT 1 FROM pg_catalog.pg_namespace n
       CROSS JOIN LATERAL pg_catalog.aclexplode(n.nspacl) acl
       JOIN pg_catalog.pg_roles grantee ON grantee.oid=acl.grantee
        WHERE grantee.rolname IN
          ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login')
          AND NOT (grantee.rolname='aimee_kb_vault_orchestrator' AND
                   n.nspname='aimee_kb_vault_orchestrator_api'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_class c
       CROSS JOIN LATERAL pg_catalog.aclexplode(c.relacl) acl
       JOIN pg_catalog.pg_roles grantee ON grantee.oid=acl.grantee
        WHERE grantee.rolname IN
          ('aimee_kb_vault_orchestrator','aimee_kb_vault_orchestrator_login'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_proc p
       CROSS JOIN LATERAL pg_catalog.aclexplode(p.proacl) acl
       JOIN pg_catalog.pg_roles grantee ON grantee.oid=acl.grantee
        WHERE grantee.rolname='aimee_kb_vault_orchestrator_login' OR
              (grantee.rolname='aimee_kb_vault_orchestrator' AND
               p.oid<>'aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status()'::regprocedure)) THEN
    RAISE EXCEPTION 'P7 D3a cross-schema direct ACL closure mismatch';
  END IF;
  IF (SELECT count(*) FROM pg_catalog.pg_auth_members membership
       JOIN pg_catalog.pg_roles member ON member.oid=membership.member
       WHERE member.rolname='aimee_kb_vault_orchestrator_login')<>1 OR
     (SELECT count(*) FROM pg_catalog.pg_auth_members membership
       JOIN pg_catalog.pg_roles member ON member.oid=membership.member
       WHERE member.rolname='aimee_kb_vault_orchestrator')<>0 OR
     (SELECT count(*) FROM pg_catalog.pg_auth_members membership
       JOIN pg_catalog.pg_roles granted ON granted.oid=membership.roleid
       WHERE granted.rolname='aimee_kb_vault_orchestrator')<>1 THEN
    RAISE EXCEPTION 'P7 D3a membership closure mismatch';
  END IF;
  IF EXISTS (
       SELECT 1 FROM pg_catalog.pg_namespace n
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND (pg_catalog.has_schema_privilege(
                 'aimee_kb_vault_orchestrator_login',n.oid,'USAGE') OR
               pg_catalog.has_schema_privilege(
                 'aimee_kb_vault_orchestrator_login',n.oid,'CREATE')))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_namespace n
       CROSS JOIN LATERAL pg_catalog.aclexplode(n.nspacl) acl
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND acl.grantee=(SELECT oid FROM pg_catalog.pg_roles
                            WHERE rolname='aimee_kb_vault_orchestrator_login'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_class c
       JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
       CROSS JOIN LATERAL pg_catalog.aclexplode(c.relacl) acl
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND acl.grantee=(SELECT oid FROM pg_catalog.pg_roles
                            WHERE rolname='aimee_kb_vault_orchestrator_login'))
     OR EXISTS (
       SELECT 1 FROM pg_catalog.pg_proc p
       JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
       CROSS JOIN LATERAL pg_catalog.aclexplode(p.proacl) acl
        WHERE pg_catalog.left(n.nspname,3)<>'pg_'
          AND n.nspname<>'information_schema'
          AND acl.grantee=(SELECT oid FROM pg_catalog.pg_roles
                            WHERE rolname='aimee_kb_vault_orchestrator_login')) THEN
    RAISE EXCEPTION 'P7 D3a authenticator application ACL closure mismatch';
  END IF;

  IF (SELECT owner.rolname FROM pg_catalog.pg_namespace n
       JOIN pg_catalog.pg_roles owner ON owner.oid=n.nspowner
      WHERE n.nspname='aimee_kb_vault_orchestrator_api')<>'aimee_kb_owner' OR
     (SELECT owner.rolname FROM pg_catalog.pg_proc p
       JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
       JOIN pg_catalog.pg_roles owner ON owner.oid=p.proowner
      WHERE n.nspname='aimee_kb_vault_orchestrator_api'
        AND p.proname='org_vault_rewrap_operator_status')<>'aimee_kb_owner' OR
     NOT EXISTS (SELECT 1 FROM pg_catalog.pg_default_acl d
      WHERE d.defaclrole=(SELECT oid FROM pg_catalog.pg_roles
                           WHERE rolname='aimee_kb_owner')
        AND d.defaclnamespace=0
        AND d.defaclobjtype='f') OR
     EXISTS (SELECT 1 FROM pg_catalog.pg_default_acl d
       CROSS JOIN LATERAL pg_catalog.aclexplode(d.defaclacl) acl
      WHERE d.defaclrole=(SELECT oid FROM pg_catalog.pg_roles
                           WHERE rolname='aimee_kb_owner')
        AND d.defaclnamespace=0
        AND d.defaclobjtype='f' AND acl.grantee=0 AND acl.privilege_type='EXECUTE') THEN
    RAISE EXCEPTION 'P7 D3a private schema ownership/default ACL mismatch';
  END IF;

  SELECT * INTO STRICT status FROM
    aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
  IF status.seal_epoch<>1 OR status.sealed OR status.control_fence<>1 OR
     status.last_opened_fence<>0 OR status.operation_id IS NOT NULL THEN
    RAISE EXCEPTION 'P7 D3a empty history mismatch';
  END IF;

  INSERT INTO public.kb_vault_rewrap_operation(operation_id,request_id,actor,state,
    seal_epoch,fencing_token,old_generation,new_generation,failure_class)
  VALUES('ffffffffffffffffffffffffffffff01','request-control',E'uid:0\n','aborted',
    1,1,0,1,'cancelled');
  BEGIN
    PERFORM * FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
    RAISE EXCEPTION 'P7 D3a accepted actor control character';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL;
  END;
  DELETE FROM public.kb_vault_rewrap_operation
   WHERE operation_id='ffffffffffffffffffffffffffffff01';
  INSERT INTO public.kb_vault_rewrap_operation(operation_id,request_id,actor,state,
    seal_epoch,fencing_token,old_generation,new_generation,failure_class)
  VALUES('ffffffffffffffffffffffffffffff02',E'request\tcontrol','uid:0','aborted',
    1,1,0,1,'cancelled');
  BEGIN
    PERFORM * FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
    RAISE EXCEPTION 'P7 D3a accepted request control character';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL;
  END;
  DELETE FROM public.kb_vault_rewrap_operation
   WHERE operation_id='ffffffffffffffffffffffffffffff02';

  INSERT INTO public.kb_vault_rewrap_operation(operation_id,request_id,actor,state,
    seal_epoch,fencing_token,old_generation,new_generation,receipt,receipt_digest,
    inventory_digest,stage_digest)
  VALUES('00000000000000000000000000000001','request-1','uid:0','completed',
    2,1,1,2,'\x01',decode(repeat('01',32),'hex'),decode(repeat('02',32),'hex'),
    decode(repeat('03',32),'hex'));
  UPDATE public.kb_vault_control SET sealed=true,seal_epoch=2,fencing_token=2;
  SELECT * INTO STRICT status FROM
    aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
  IF status.operation_state<>'completed' OR status.operation_fence<>1 THEN
    RAISE EXCEPTION 'P7 D3a completed obligation mismatch';
  END IF;

  UPDATE public.kb_vault_control SET fencing_token=3;
  BEGIN
    PERFORM * FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
    RAISE EXCEPTION 'P7 D3a accepted gapped terminal control fence';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL;
  END;
  UPDATE public.kb_vault_control SET fencing_token=2,seal_epoch=3;
  BEGIN
    PERFORM * FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
    RAISE EXCEPTION 'P7 D3a accepted terminal seal epoch mismatch';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL;
  END;
  UPDATE public.kb_vault_control SET seal_epoch=2;

  UPDATE public.kb_vault_control SET sealed=false,last_opened_rewrap_fence=1;
  SELECT * INTO STRICT status FROM
    aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
  IF status.operation_id IS NOT NULL OR status.last_opened_fence<>1 THEN
    RAISE EXCEPTION 'P7 D3a opened marker mismatch';
  END IF;

  INSERT INTO public.kb_vault_rewrap_operation(operation_id,request_id,actor,state,
    seal_epoch,fencing_token,old_generation,new_generation,failure_class,failure_from_state)
  VALUES('00000000000000000000000000000002','request-2','uid:0','recovery_required',
    3,3,2,3,'backend','preparing');
  UPDATE public.kb_vault_control SET sealed=true,seal_epoch=3,fencing_token=4;
  SELECT * INTO STRICT status FROM
    aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
  IF status.operation_state<>'recovery_required' OR status.failure_class<>'backend' THEN
    RAISE EXCEPTION 'P7 D3a recovery obligation mismatch';
  END IF;
END
$test$;

-- The authenticator cannot invoke or even resolve the facade before the
-- explicit SET ROLE.  The capability can invoke exactly the qualified facade.
SET ROLE aimee_kb_vault_orchestrator_login;
DO $denied$
BEGIN
  BEGIN
    PERFORM * FROM
      aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
    RAISE EXCEPTION 'P7 D3a authenticator directly invoked private facade';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END
$denied$;
SET ROLE aimee_kb_vault_orchestrator;
DO $allowed$
DECLARE status RECORD;
BEGIN
  SELECT * INTO STRICT status FROM
    aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
  IF status.operation_state<>'recovery_required' THEN
    RAISE EXCEPTION 'P7 D3a capability facade result mismatch';
  END IF;
END
$allowed$;
RESET ROLE;

-- A later aborted row may not hide an earlier outstanding recovery obligation.
INSERT INTO public.kb_vault_rewrap_operation(operation_id,request_id,actor,state,
  seal_epoch,fencing_token,old_generation,new_generation,failure_class)
VALUES('00000000000000000000000000000003','request-3','uid:0','aborted',
  4,5,3,4,'cancelled');
UPDATE public.kb_vault_control SET seal_epoch=4,fencing_token=6;
DO $test$
DECLARE
  returned_state TEXT;
BEGIN
  BEGIN
    PERFORM * FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
    RAISE EXCEPTION 'P7 D3a accepted hidden recovery obligation';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN
    GET STACKED DIAGNOSTICS returned_state=RETURNED_SQLSTATE;
    IF returned_state<>'55000' THEN
      RAISE EXCEPTION 'P7 D3a integrity SQLSTATE mismatch: %',returned_state;
    END IF;
  END;
END
$test$;

ROLLBACK;
