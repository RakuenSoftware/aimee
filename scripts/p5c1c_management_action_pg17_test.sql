\set ON_ERROR_STOP on

BEGIN;

INSERT INTO public.kb_team(id,name) VALUES(9751,'p5c1c-team-a'),(9752,'p5c1c-team-b');
INSERT INTO public.kb_team_membership(identity_key,team,is_default) VALUES
  ('oidc:https%3A%25issuer:lead',9751,1),('oidc:https%3A%25issuer:member',9751,1),
  ('oidc:https%3A%25issuer:other',9752,1);
INSERT INTO public.kb_team_lead(identity_key,team,granted_by)
  VALUES('oidc:https%3A%25issuer:lead',9751,'owner'),
        ('oidc:https%3A%25issuer:other',9752,'owner');

INSERT INTO public.kb_enrollments(id,scope,fingerprint,serial,state,expires_at,revoked_at,
  authority_id,cert_issuer,cert_serial_norm) VALUES
  (97511,'p5-kb-management',repeat('1',64),'11','active',
    to_char(now()+interval '1 hour','YYYY-MM-DD HH24:MI:SS'),'',repeat('a',32),
    '/CN=p5c1c-local-ca','11'),
  (97512,'p5-server-management',repeat('2',64),'22','active',
    to_char(now()+interval '1 hour','YYYY-MM-DD HH24:MI:SS'),'',repeat('b',32),
    '/CN=p5c1c-target-ca','22');
INSERT INTO public.kb_server_registry(server_id,cert_cn,mgmt_cert_cn,owner_issuer,
  owner_subject,team_id,endpoint,status,mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
VALUES('srv-p5c1c','server:srv-p5c1c:client','server:srv-p5c1c:management','test','owner',
  9751,'https://192.0.2.1:7443','active','/CN=p5c1c-target-ca','22',repeat('2',64));
INSERT INTO public.kb_management_instance_grant(installation_id,replacement_lineage_id,
  team_id,workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
  expected_ca_issuer,expected_ca_fingerprint,creator_identity,state,consumed_at)
VALUES(repeat('3',32),repeat('3',32),9751,'spiffe://p5c1c.test','kb-node',repeat('4',64),
  repeat('5',64),repeat('6',64),'/CN=p5c1c-local-ca',repeat('7',64),'owner','consumed',now());
INSERT INTO public.kb_management_instance(installation_id,replacement_lineage_id,authority_id,
  team_id,workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
  expected_ca_issuer,expected_ca_fingerprint,current_generation,current_enrollment_id,state)
VALUES(repeat('3',32),repeat('3',32),repeat('a',32),9751,'spiffe://p5c1c.test','kb-node',
  repeat('4',64),repeat('5',64),repeat('6',64),'/CN=p5c1c-local-ca',repeat('7',64),1,97511,'active');
INSERT INTO public.kb_management_instance_issue(operation_id,installation_id,issue_kind,
  generation,csr_digest,csr_spki_digest,public_bundle_digest,cert_issuer,cert_serial_norm,
  cert_fingerprint,cert_spki_digest,cert_not_before,cert_not_after,enrollment_id,state,
  created_at,pending_expires_at,activated_at)
VALUES(repeat('8',64),repeat('3',32),'initial',1,repeat('9',64),repeat('a',64),repeat('b',64),
  '/CN=p5c1c-local-ca','11',repeat('1',64),repeat('a',64),now()-interval '1 minute',
  now()+interval '1 hour',97511,'active',now()-interval '2 minutes',
  now()-interval '1 minute',now()-interval '30 seconds');

DO $$
DECLARE t TEXT; n INTEGER; f REGPROCEDURE;
BEGIN
  FOREACH t IN ARRAY ARRAY['kb_management_action_intent','kb_management_action_outcome'] LOOP
    SELECT count(*) INTO n FROM pg_catalog.pg_class c
      JOIN pg_catalog.pg_namespace s ON s.oid=c.relnamespace
      WHERE s.nspname='public' AND c.relname=t AND c.relrowsecurity AND c.relforcerowsecurity
        AND pg_catalog.pg_get_userbyid(c.relowner)='aimee_kb_owner';
    IF n<>1 THEN RAISE EXCEPTION 'owner/RLS mismatch for %',t; END IF;
    IF has_table_privilege('aimee_kb_runtime','public.'||t,'SELECT') OR
       has_table_privilege('aimee_kb_runtime','public.'||t,'INSERT') OR
       has_table_privilege('aimee_kb_runtime','public.'||t,'UPDATE') OR
       has_table_privilege('aimee_kb_runtime','public.'||t,'DELETE') OR
       has_table_privilege('aimee_kb_runtime','public.'||t,'TRUNCATE') THEN
      RAISE EXCEPTION 'runtime direct journal privilege on %',t;
    END IF;
  END LOOP;
  IF NOT has_function_privilege('aimee_kb_runtime',
       'public.kb_management_action_intent_start(text,text,bigint,text,text,text,text,text,integer,text)','EXECUTE') OR
     NOT has_function_privilege('aimee_kb_runtime',
       'public.kb_management_action_outcome_append(text,text,text,integer,text)','EXECUTE') OR
     has_function_privilege('aimee_kb_runtime',
       'public.kb_audit_worm_append(text,text,text,text,text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'runtime facade/audit ACL mismatch';
  END IF;
  FOREACH f IN ARRAY ARRAY[
    'public.kb_management_action_intent_start(text,text,bigint,text,text,text,text,text,integer,text)'::REGPROCEDURE,
    'public.kb_management_action_outcome_append(text,text,text,integer,text)'::REGPROCEDURE] LOOP
    SELECT count(*) INTO n FROM pg_catalog.pg_proc p,
      LATERAL pg_catalog.aclexplode(COALESCE(p.proacl,pg_catalog.acldefault('f',p.proowner))) a
      WHERE p.oid=f AND a.grantee=0 AND a.privilege_type='EXECUTE';
    IF n<>0 THEN RAISE EXCEPTION 'PUBLIC execute on %',f; END IF;
  END LOOP;
END $$;

SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:lead',9751);
DO $$
DECLARE r RECORD; a0 BIGINT;
BEGIN
  SELECT count(*) INTO a0 FROM public.kb_audit_outbox;
  SELECT * INTO r FROM public.kb_management_action_intent_start(
    repeat('c',64),repeat('d',64),9751,'srv-p5c1c','remote_writes',repeat('e',64),
    'https://kb.p5c1c.test','kid-1',90,repeat('3',32));
  IF r.replayed OR r.actor_identity<>'oidc:https%3A%25issuer:lead' OR
     r.audience<>'srv-p5c1c' OR r.expires_at-r.issued_at<>90 OR
     r.installation_generation<>1 OR r.installation_enrollment_id<>97511 OR
     r.target_enrollment_id<>97512 OR r.local_cert_fingerprint<>repeat('1',64) OR
     r.target_mgmt_fingerprint<>repeat('2',64) THEN
    RAISE EXCEPTION 'intent snapshot mismatch';
  END IF;
  IF (SELECT count(*) FROM public.kb_audit_outbox)<>a0+1 THEN
    RAISE EXCEPTION 'intent WORM append mismatch';
  END IF;
  SELECT * INTO r FROM public.kb_management_action_intent_start(
    repeat('c',64),repeat('d',64),9751,'srv-p5c1c','remote_writes',repeat('e',64),
    'https://kb.p5c1c.test','kid-1',90,repeat('3',32));
  IF NOT r.replayed OR (SELECT count(*) FROM public.kb_audit_outbox)<>a0+1 THEN
    RAISE EXCEPTION 'exact intent replay did not converge';
  END IF;
  BEGIN
    PERFORM * FROM public.kb_management_action_intent_start(
      repeat('c',64),repeat('d',64),9751,'srv-p5c1c','remote_writes',repeat('e',64),
      'https://kb.p5c1c.test','kid-1',89,repeat('3',32));
    RAISE EXCEPTION 'TTL mutation replayed';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_action_intent_start(
      repeat('f',64),repeat('d',64),9751,'srv-p5c1c','remote_writes',repeat('e',64),
      'https://kb.p5c1c.test','kid-1',90,repeat('3',32));
    RAISE EXCEPTION 'JTI reused across correlation';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_action_intent_start(
      NULL,repeat('a',64),9751,'srv-p5c1c','remote_writes',repeat('e',64),
      'https://kb.p5c1c.test','kid-1',90,repeat('3',32));
    RAISE EXCEPTION 'NULL correlation accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;

DO $$
DECLARE r RECORD; a0 BIGINT;
BEGIN
  SELECT count(*) INTO a0 FROM public.kb_audit_outbox;
  SELECT * INTO r FROM public.kb_management_action_outcome_append(
    repeat('c',64),'succeeded','remote_success',200,repeat('f',64));
  IF r.replayed OR r.result<>'succeeded' THEN RAISE EXCEPTION 'outcome mismatch'; END IF;
  SELECT * INTO r FROM public.kb_management_action_outcome_append(
    repeat('c',64),'succeeded','remote_success',200,repeat('f',64));
  IF NOT r.replayed OR (SELECT count(*) FROM public.kb_audit_outbox)<>a0+1 THEN
    RAISE EXCEPTION 'exact outcome replay did not converge';
  END IF;
  BEGIN
    PERFORM * FROM public.kb_management_action_outcome_append(
      repeat('c',64),'failed','remote_failure',500,NULL);
    RAISE EXCEPTION 'terminal outcome replaced';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;
RESET ROLE;

-- Ordinary members and a lead from another team create neither journal nor WORM rows.
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:member',9751);
DO $$ DECLARE a0 BIGINT; BEGIN
  SELECT count(*) INTO a0 FROM public.kb_audit_outbox;
  BEGIN
    PERFORM * FROM public.kb_management_action_intent_start(
      repeat('a',64),repeat('b',64),9751,'srv-p5c1c','remote_writes',repeat('c',64),
      'https://kb.p5c1c.test','kid-1',30,repeat('3',32));
    RAISE EXCEPTION 'ordinary member admitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  IF (SELECT count(*) FROM public.kb_audit_outbox)<>a0 THEN RAISE EXCEPTION 'member denial audited'; END IF;
END $$;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:other',9752);
DO $$ DECLARE a0 BIGINT; BEGIN
  SELECT count(*) INTO a0 FROM public.kb_audit_outbox;
  BEGIN
    PERFORM * FROM public.kb_management_action_intent_start(
      repeat('a',64),repeat('b',64),9752,'srv-p5c1c','remote_writes',repeat('c',64),
      'https://kb.p5c1c.test','kid-1',30,repeat('3',32));
    RAISE EXCEPTION 'cross-team target admitted';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
  IF (SELECT count(*) FROM public.kb_audit_outbox)<>a0 THEN RAISE EXCEPTION 'cross-team denial audited'; END IF;
END $$;
RESET ROLE;

-- Mutable authority is rechecked for every new correlation.  Inactive target,
-- revoked target certificate, and expired local certificate all fail atomically.
UPDATE public.kb_server_registry SET status='pending' WHERE server_id='srv-p5c1c';
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:lead',9751);
DO $$ DECLARE a0 BIGINT; BEGIN
  SELECT count(*) INTO a0 FROM public.kb_audit_outbox;
  BEGIN PERFORM * FROM public.kb_management_action_intent_start(
    repeat('1',64),repeat('2',64),9751,'srv-p5c1c','remote_writes',repeat('3',64),
    'https://kb.p5c1c.test','kid-1',30,repeat('3',32));
    RAISE EXCEPTION 'inactive target admitted';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
  IF (SELECT count(*) FROM public.kb_audit_outbox)<>a0 THEN RAISE EXCEPTION 'inactive denial audited'; END IF;
END $$;
RESET ROLE;
UPDATE public.kb_server_registry SET status='active' WHERE server_id='srv-p5c1c';

INSERT INTO public.kb_enrollments(id,scope,fingerprint,serial,state,expires_at,revoked_at,
  authority_id,cert_issuer,cert_serial_norm)
VALUES(97513,'p5-server-management',repeat('0',64),'33','revoked',
  to_char(now()+interval '1 hour','YYYY-MM-DD HH24:MI:SS'),now()::TEXT,repeat('0',32),
  '/CN=p5c1c-revoked-ca','33');
UPDATE public.kb_server_registry SET mgmt_issuer='/CN=p5c1c-revoked-ca',mgmt_serial_norm='33',
  mgmt_fingerprint=repeat('0',64) WHERE server_id='srv-p5c1c';
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:lead',9751);
DO $$ DECLARE a0 BIGINT; BEGIN
  SELECT count(*) INTO a0 FROM public.kb_audit_outbox;
  BEGIN PERFORM * FROM public.kb_management_action_intent_start(
    repeat('2',64),repeat('3',64),9751,'srv-p5c1c','remote_writes',repeat('4',64),
    'https://kb.p5c1c.test','kid-1',30,repeat('3',32));
    RAISE EXCEPTION 'revoked target certificate admitted';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
  IF (SELECT count(*) FROM public.kb_audit_outbox)<>a0 THEN RAISE EXCEPTION 'revoked denial audited'; END IF;
END $$;
RESET ROLE;
UPDATE public.kb_server_registry SET mgmt_issuer='/CN=p5c1c-target-ca',mgmt_serial_norm='22',
  mgmt_fingerprint=repeat('2',64) WHERE server_id='srv-p5c1c';

UPDATE public.kb_enrollments SET expires_at=to_char(now()-interval '1 minute','YYYY-MM-DD HH24:MI:SS')
  WHERE id=97511;
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:lead',9751);
DO $$ DECLARE a0 BIGINT; BEGIN
  SELECT count(*) INTO a0 FROM public.kb_audit_outbox;
  BEGIN PERFORM * FROM public.kb_management_action_intent_start(
    repeat('3',64),repeat('4',64),9751,'srv-p5c1c','remote_writes',repeat('5',64),
    'https://kb.p5c1c.test','kid-1',30,repeat('3',32));
    RAISE EXCEPTION 'expired local certificate admitted';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
  IF (SELECT count(*) FROM public.kb_audit_outbox)<>a0 THEN RAISE EXCEPTION 'expiry denial audited'; END IF;
END $$;
RESET ROLE;
UPDATE public.kb_enrollments SET expires_at=to_char(now()+interval '1 hour','YYYY-MM-DD HH24:MI:SS')
  WHERE id=97511;

-- The outcome verdict is the terminal result; audit detail remains a strict
-- metadata allowlist and contains no endpoint, payload, bearer, or token body.
DO $$
DECLARE d TEXT;
BEGIN
  SELECT detail INTO d FROM public.kb_audit_outbox
    WHERE action='management.action.intent' AND subject=repeat('c',64);
  IF d IS NULL OR d LIKE '%endpoint%' OR d LIKE '%request_body%' OR d LIKE '%response_body%' OR
     d LIKE '%authorization%' OR d LIKE '%jwt%' THEN
    RAISE EXCEPTION 'intent audit detail allowlist violated';
  END IF;
  IF NOT EXISTS(SELECT 1 FROM public.kb_audit_outbox WHERE action='management.action.outcome'
      AND subject=repeat('c',64) AND verdict='succeeded') THEN
    RAISE EXCEPTION 'outcome audit verdict mismatch';
  END IF;
END $$;

-- Even the owner cannot rewrite or remove history.
DO $$ BEGIN
  BEGIN UPDATE public.kb_management_action_intent SET kid='changed' WHERE correlation_id=repeat('c',64);
    RAISE EXCEPTION 'intent update succeeded'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN DELETE FROM public.kb_management_action_outcome WHERE correlation_id=repeat('c',64);
    RAISE EXCEPTION 'outcome delete succeeded'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN TRUNCATE public.kb_management_action_intent,public.kb_management_action_outcome;
    RAISE EXCEPTION 'intent truncate succeeded'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
END $$;

-- Failure of the WORM append rolls back the structured intent as one unit.
CREATE FUNCTION pg_temp.p5c1c_fail_audit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.action='management.action.intent' AND NEW.subject=repeat('9',64) THEN
    RAISE EXCEPTION 'injected WORM failure';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER aa_p5c1c_fail_audit BEFORE INSERT ON public.kb_audit_outbox
  FOR EACH ROW EXECUTE FUNCTION pg_temp.p5c1c_fail_audit();
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:lead',9751);
DO $$ BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_action_intent_start(
      repeat('9',64),repeat('8',64),9751,'srv-p5c1c','remote_writes',repeat('7',64),
      'https://kb.p5c1c.test','kid-1',30,repeat('3',32));
    RAISE EXCEPTION 'injected audit failure accepted';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM<>'injected WORM failure' THEN RAISE; END IF;
  END;
END $$;
RESET ROLE;
DROP TRIGGER aa_p5c1c_fail_audit ON public.kb_audit_outbox;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_management_token_intent_namespace
       WHERE correlation_id=repeat('9',64)) OR
     EXISTS(SELECT 1 FROM public.kb_management_action_intent WHERE correlation_id=repeat('9',64)) OR
     EXISTS(SELECT 1 FROM public.kb_audit_outbox WHERE action='management.action.intent'
       AND subject=repeat('9',64)) THEN
    RAISE EXCEPTION 'atomic intent/WORM rollback failed';
  END IF;
END $$;

-- A committed intent without an outcome remains explicitly unresolved.
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:lead',9751);
SELECT * FROM public.kb_management_action_intent_start(
  repeat('6',64),repeat('5',64),9751,'srv-p5c1c','remote_writes',repeat('4',64),
  'https://kb.p5c1c.test','kid-1',30,repeat('3',32));
RESET ROLE;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_management_action_outcome WHERE correlation_id=repeat('6',64)) THEN
    RAISE EXCEPTION 'intent-only fixture gained an outcome';
  END IF;
  IF (SELECT ROW(n.jti,n.kind) FROM public.kb_management_token_intent_namespace n
        WHERE n.correlation_id=repeat('6',64)) IS DISTINCT FROM
          ROW(repeat('5',64),'action'::TEXT) THEN
    RAISE EXCEPTION 'action namespace tuple mismatch';
  END IF;
  IF has_table_privilege('aimee_kb_runtime',
       'public.kb_management_token_intent_namespace','SELECT') OR
     has_table_privilege('aimee_kb_runtime',
       'public.kb_management_token_intent_namespace','INSERT') THEN
    RAISE EXCEPTION 'runtime gained direct namespace privilege';
  END IF;
  BEGIN
    INSERT INTO public.kb_management_token_intent_namespace(correlation_id,jti,kind)
      VALUES(repeat('6',64),repeat('7',64),'read');
    RAISE EXCEPTION 'cross-kind correlation collision accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN
    UPDATE public.kb_management_token_intent_namespace SET kind='read'
      WHERE correlation_id=repeat('6',64);
    RAISE EXCEPTION 'namespace update accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
END $$;

-- Failure of the outcome WORM append likewise rolls back the terminal row.
CREATE FUNCTION pg_temp.p5c1c_fail_outcome_audit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.action='management.action.outcome' AND NEW.subject=repeat('6',64) THEN
    RAISE EXCEPTION 'injected outcome WORM failure';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER aa_p5c1c_fail_outcome_audit BEFORE INSERT ON public.kb_audit_outbox
  FOR EACH ROW EXECUTE FUNCTION pg_temp.p5c1c_fail_outcome_audit();
SET LOCAL ROLE aimee_kb_runtime;
SELECT public.set_tenant_context('oidc:https%3A%25issuer:lead',9751);
DO $$ BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_action_outcome_append(
      repeat('6',64),'failed','local_failure',NULL,NULL);
    RAISE EXCEPTION 'injected outcome audit failure accepted';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM<>'injected outcome WORM failure' THEN RAISE; END IF;
  END;
  BEGIN
    PERFORM * FROM public.kb_management_action_outcome_append(
      repeat('6',64),NULL,'local_failure',NULL,NULL);
    RAISE EXCEPTION 'NULL outcome result accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
RESET ROLE;
DROP TRIGGER aa_p5c1c_fail_outcome_audit ON public.kb_audit_outbox;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_management_action_outcome
      WHERE correlation_id=repeat('6',64)) OR
     EXISTS(SELECT 1 FROM public.kb_audit_outbox WHERE action='management.action.outcome'
       AND subject=repeat('6',64)) THEN
    RAISE EXCEPTION 'atomic outcome/WORM rollback failed';
  END IF;
END $$;

ROLLBACK;

BEGIN READ ONLY;
DO $$ BEGIN
  PERFORM set_config('aimee.principal','oidc:https%3A%25issuer:lead',true);
  PERFORM set_config('aimee.team','9751',true);
  BEGIN
    PERFORM * FROM public.kb_management_action_intent_start(
      repeat('1',64),repeat('2',64),9751,'srv-p5c1c','remote_writes',repeat('3',64),
      'https://kb.p5c1c.test','kid-1',30,repeat('4',32));
    RAISE EXCEPTION 'read-only admission accepted';
  EXCEPTION WHEN read_only_sql_transaction THEN NULL; END;
END $$;
ROLLBACK;

SELECT 'p5c1c_management_action_pg17: ok';
