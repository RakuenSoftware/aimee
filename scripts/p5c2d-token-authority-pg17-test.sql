\set ON_ERROR_STOP on

-- P5-C2d focused PostgreSQL 17 shape, role and fail-closed gate.  Run after
-- schema_roles.sql, schema.sql and schema_grants.sql in a throwaway database.
BEGIN;

DO $$
DECLARE fn REGPROCEDURE; role_name TEXT; definer_oid OID; runtime_oid OID; store_oid OID;
BEGIN
  SELECT oid INTO definer_oid FROM pg_roles WHERE rolname='aimee_kb_token_authority_definer'
    AND NOT rolcanlogin AND NOT rolinherit AND rolbypassrls AND NOT rolsuper
    AND NOT rolcreatedb AND NOT rolcreaterole AND NOT rolreplication;
  SELECT oid INTO runtime_oid FROM pg_roles WHERE rolname='aimee_kb_token_authority_runtime'
    AND rolcanlogin AND NOT rolinherit AND NOT rolbypassrls AND NOT rolsuper
    AND NOT rolcreatedb AND NOT rolcreaterole AND NOT rolreplication;
  SELECT oid INTO store_oid FROM pg_roles WHERE rolname='aimee_kb_token_authority_store_owner'
    AND NOT rolcanlogin AND NOT rolinherit AND NOT rolbypassrls AND NOT rolsuper
    AND NOT rolcreatedb AND NOT rolcreaterole AND NOT rolreplication;
  IF definer_oid IS NULL OR runtime_oid IS NULL OR store_oid IS NULL THEN
    RAISE EXCEPTION 'P5-C2d role posture mismatch';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_catalog.pg_auth_members am
       WHERE am.roleid IN (definer_oid,runtime_oid,store_oid)
          OR am.member IN (definer_oid,runtime_oid,store_oid)
          OR am.grantor IN (definer_oid,runtime_oid,store_oid)) OR
     (SELECT relowner<>store_oid FROM pg_class
       WHERE oid='public.kb_management_token_key_use_intent'::regclass) THEN
    RAISE EXCEPTION 'P5-C2d definer inheritance escaped';
  END IF;

  FOREACH fn IN ARRAY ARRAY[
    'public.kb_management_token_authority_admit(text,text)'::regprocedure,
    'public.kb_management_token_authority_use(text,text)'::regprocedure,
    'public.kb_management_token_authority_readback(text,text)'::regprocedure,
    'public.kb_management_token_authority_finalize(text,text)'::regprocedure]
  LOOP
    IF NOT has_function_privilege('aimee_kb_token_authority_runtime',fn,'EXECUTE') OR
       has_function_privilege('aimee_kb_runtime',fn,'EXECUTE') OR EXISTS (
        SELECT 1 FROM pg_proc p,
          LATERAL aclexplode(COALESCE(p.proacl,acldefault('f',p.proowner))) a
         WHERE p.oid=fn AND a.grantee=0 AND a.privilege_type='EXECUTE') OR NOT EXISTS (
        SELECT 1 FROM pg_proc p WHERE p.oid=fn AND p.proowner=definer_oid
          AND p.prosecdef AND p.provolatile='v'
          AND p.proconfig @> ARRAY['search_path=pg_catalog, pg_temp']::TEXT[]) THEN
      RAISE EXCEPTION 'P5-C2d function posture mismatch: %',fn;
    END IF;
  END LOOP;

  IF NOT EXISTS (SELECT 1 FROM pg_proc p
      WHERE p.oid='public.kb_management_token_authority_snapshot(text,text)'::regprocedure
        AND p.proowner=definer_oid AND p.prosecdef AND p.provolatile='v'
        AND p.proconfig @> ARRAY['search_path=pg_catalog, pg_temp','TimeZone=UTC']::TEXT[]) THEN
    RAISE EXCEPTION 'P5-C2d snapshot search path/timezone posture mismatch';
  END IF;

  IF (SELECT count(*) FROM pg_proc p WHERE p.oid=ANY(ARRAY[
       'public.kb_management_token_authority_admit(text,text)'::regprocedure,
       'public.kb_management_token_authority_use(text,text)'::regprocedure,
       'public.kb_management_token_authority_readback(text,text)'::regprocedure])
       AND p.pronargs=2 AND p.proretset AND cardinality(p.proallargtypes)=44)<>3 THEN
    RAISE EXCEPTION 'P5-C2d 42-column record contract mismatch';
  END IF;

  IF has_table_privilege('aimee_kb_token_authority_runtime',
       'public.kb_management_token_key_use_intent','SELECT') OR
     has_table_privilege('aimee_kb_token_authority_runtime',
       'public.kb_management_token_key_use_intent','INSERT') OR
     has_table_privilege('aimee_kb_token_authority_runtime',
       'public.org_vault_secret','SELECT') OR
     has_function_privilege('aimee_kb_token_authority_runtime',
       'public.kb_management_token_authority_snapshot(text,text)','EXECUTE') OR
     has_function_privilege('aimee_kb_token_authority_runtime',
       'public.kb_audit_worm_append(text,text,text,text,text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'P5-C2d runtime escaped fixed facade';
  END IF;

  FOREACH role_name IN ARRAY ARRAY['aimee_kb_runtime','aimee_kb_status',
    'aimee_kb_status_definer','aimee_kb_status_login','aimee_kb_token_roots_provision',
    'aimee_kb_jwks_publish','aimee_kb_jwks_runtime_definer','aimee_kb_migrate'] LOOP
    IF has_table_privilege(role_name,'public.kb_management_token_key_use_intent','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_token_key_use_intent','INSERT') OR
       has_function_privilege(role_name,
         'public.kb_management_token_authority_admit(text,text)','EXECUTE') THEN
      RAISE EXCEPTION 'P5-C2d unrelated role % has authority',role_name;
    END IF;
  END LOOP;
END $$;

SET LOCAL ROLE aimee_kb_token_authority_runtime;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_token_authority_admit('x','y');
    RAISE EXCEPTION 'malformed admission accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    INSERT INTO public.kb_management_token_key_use_intent(
      correlation_id,jti,team_id,actor_identity,target_server_id,request_sha256,kid,
      token_custody_key_id,token_version,publication_generation,publication_candidate_id,
      publication_manifest_sha256,publication_envelope_sha256,vault_seal_epoch,
      hwm_attestation_digest,purpose)
    VALUES(repeat('1',64),repeat('2',64),1,'actor','server',repeat('3',64),'kid',
      'custody',2,1,repeat('4',64),decode(repeat('5',64),'hex'),
      decode(repeat('6',64),'hex'),1,decode(repeat('7',64),'hex'),
      'management.token.sign.v1');
    RAISE EXCEPTION 'direct key-use insert accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_token_authority_use(repeat('1',64),repeat('2',64));
    RAISE EXCEPTION 'READ COMMITTED use accepted';
  EXCEPTION WHEN active_sql_transaction THEN NULL; END;
END $$;
RESET ROLE;

-- WORM guards remain load-bearing even for an owner/superuser path.
INSERT INTO public.kb_team(id,name) VALUES(97521,'p5c2d-worm-team');
INSERT INTO public.kb_management_token_key_use_intent(
  correlation_id,jti,team_id,actor_identity,target_server_id,request_sha256,kid,
  token_custody_key_id,token_version,publication_generation,publication_candidate_id,
  publication_manifest_sha256,publication_envelope_sha256,vault_seal_epoch,
  hwm_attestation_digest,purpose)
VALUES(repeat('a',64),repeat('b',64),97521,'actor','server',repeat('c',64),'kid',
  'custody',2,1,repeat('d',64),decode(repeat('11',32),'hex'),
  decode(repeat('12',32),'hex'),1,decode(repeat('13',32),'hex'),
  'management.token.sign.v1');
DO $$ BEGIN
  BEGIN
    UPDATE public.kb_management_token_key_use_intent SET kid='changed'
      WHERE correlation_id=repeat('a',64);
    RAISE EXCEPTION 'key-use mutation accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN
    DELETE FROM public.kb_management_token_key_use_intent
      WHERE correlation_id=repeat('a',64);
    RAISE EXCEPTION 'key-use delete accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN
    TRUNCATE public.kb_management_token_key_use_intent;
    RAISE EXCEPTION 'key-use truncate accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
END $$;

ROLLBACK;

-- Exercise one complete admitted/use/finalize/readback tuple against real row
-- locks and FORCE-RLS state.  The fixture bypasses only offline provisioning;
-- every online call runs as the dedicated authority runtime.
BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;

INSERT INTO public.kb_team(id,name) VALUES(97522,'p5c2d-live-team');
INSERT INTO public.kb_team_membership(identity_key,team,is_default)
  VALUES('oidc:https%3A%25issuer:p5c2d-lead',97522,1);
INSERT INTO public.kb_team_lead(identity_key,team,granted_by)
  VALUES('oidc:https%3A%25issuer:p5c2d-lead',97522,'owner');
INSERT INTO public.kb_enrollments(id,scope,fingerprint,serial,state,expires_at,revoked_at,
  authority_id,cert_issuer,cert_serial_norm) VALUES
 (97521,'p5-kb-management',repeat('1',64),'11','active',
  to_char(clock_timestamp()+interval '1 hour','YYYY-MM-DD HH24:MI:SS'),'',repeat('a',32),
  '/CN=p5c2d-local-ca','11'),
 (97522,'p5-server-management',repeat('2',64),'22','active',
  to_char(clock_timestamp()+interval '1 hour','YYYY-MM-DD HH24:MI:SS'),'',repeat('b',32),
  '/CN=p5c2d-target-ca','22');
INSERT INTO public.kb_server_registry(server_id,cert_cn,mgmt_cert_cn,owner_issuer,
  owner_subject,team_id,endpoint,status,mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
VALUES('srv-p5c2d','server:srv-p5c2d:client','server:srv-p5c2d:management','test','owner',
  97522,'https://192.0.2.2:7443','active','/CN=p5c2d-target-ca','22',repeat('2',64));
INSERT INTO public.kb_management_instance_grant(installation_id,replacement_lineage_id,
  team_id,workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
  expected_ca_issuer,expected_ca_fingerprint,creator_identity,state,consumed_at)
VALUES(repeat('3',32),repeat('3',32),97522,'spiffe://p5c2d.test','kb-node',repeat('4',64),
  repeat('5',64),repeat('6',64),'/CN=p5c2d-local-ca',repeat('7',64),'owner','consumed',now());
INSERT INTO public.kb_management_instance(installation_id,replacement_lineage_id,authority_id,
  team_id,workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
  expected_ca_issuer,expected_ca_fingerprint,current_generation,current_enrollment_id,state)
VALUES(repeat('3',32),repeat('3',32),repeat('a',32),97522,'spiffe://p5c2d.test','kb-node',
  repeat('4',64),repeat('5',64),repeat('6',64),'/CN=p5c2d-local-ca',repeat('7',64),1,97521,'active');
INSERT INTO public.kb_management_instance_issue(operation_id,installation_id,issue_kind,
  generation,csr_digest,csr_spki_digest,public_bundle_digest,cert_issuer,cert_serial_norm,
  cert_fingerprint,cert_spki_digest,cert_not_before,cert_not_after,enrollment_id,state,
  created_at,pending_expires_at,activated_at)
VALUES(repeat('8',64),repeat('3',32),'initial',1,repeat('9',64),repeat('a',64),repeat('b',64),
  '/CN=p5c2d-local-ca','11',repeat('1',64),repeat('a',64),now()-interval '1 minute',
  now()+interval '1 hour',97521,'active',now()-interval '2 minutes',
  now()-interval '1 minute',now()-interval '30 seconds');

CREATE TEMP TABLE p5c2d_key AS WITH k AS (
 SELECT decode('01'||repeat('31',383),'hex') public_key), b AS (
 SELECT public_key,'p5-token-v1-'||substr(encode(sha256(convert_to(
  'aimee.p5.token.public.v1'||chr(10),'UTF8')||int4send(384)||public_key||
  int4send(3)||decode('010001','hex')),'hex'),1,32) wire FROM k)
SELECT public_key,wire,sha256(public_key) public_digest,
 sha256(convert_to('{"kty":"RSA","kid":"'||wire||'","use":"sig","alg":"RS256","n":"'||
 rtrim(replace(replace(replace(encode(public_key,'base64'),chr(10),''),'+','-'),'/','_'),'=')||
 '","e":"AQAB"}','UTF8')) jwk_digest FROM b;

INSERT INTO public.kb_management_token_root_vault_permit
  VALUES(pg_backend_pid(),txid_current(),'token');
INSERT INTO public.org_vault_secret(principal,team_id,agent,cred,version,wrapped_dek,nonce,
  ciphertext,tag,hwm_attestation) VALUES
 ('org:p5-token',NULL,'management','rs256',1,decode(repeat('11',40),'hex'),
  decode(repeat('12',12),'hex'),decode(repeat('13',64),'hex'),decode(repeat('14',16),'hex'),
  decode(repeat('15',64),'hex')),
 ('org:p5-token',NULL,'management','rs256',2,decode(repeat('21',40),'hex'),
  decode(repeat('22',12),'hex'),decode(repeat('23',64),'hex'),decode(repeat('24',16),'hex'),
  decode(repeat('25',64),'hex'));
INSERT INTO public.org_vault_current
  VALUES('org:p5-token','management','rs256',2,public.pg_now_text());
INSERT INTO public.org_vault_rotation(key_id,principal,team_id,agent,cred,from_version,
  to_version,state,hwm_attestation)
 VALUES('p5c2d-token-custody','org:p5-token',NULL,'management','rs256',1,2,'activated',
   decode(repeat('25',64),'hex'));
DELETE FROM public.kb_management_token_root_vault_permit;
INSERT INTO public.kb_management_token_root(root_kind,bootstrap_id,custody_key_id,wire_id,
  public_key,public_exponent,public_digest,jwk_digest,current_version,initial_seal_epoch,
  v1_envelope_digest,v2_envelope_digest,hwm2_attestation_digest,enabled)
SELECT 'token',repeat('e',64),'p5c2d-token-custody',wire,public_key,decode('010001','hex'),
 public_digest,jwk_digest,2,1,decode(repeat('41',32),'hex'),decode(repeat('42',32),'hex'),
 sha256(decode(repeat('25',64),'hex')),true FROM p5c2d_key;

CREATE TEMP TABLE p5c2d_pub AS SELECT repeat('f',64)::TEXT candidate_id,
 floor(extract(epoch FROM clock_timestamp()))::BIGINT-10 valid_from,
 floor(extract(epoch FROM clock_timestamp()))::BIGINT+300 valid_until,
 convert_to('{"keys":[]}','UTF8') jwks_bytes,
 convert_to('{"generation":1}','UTF8') payload_bytes,
 convert_to('{"manifest":1}','UTF8') envelope_bytes,
 decode(repeat('51',32),'hex') manifest_sha256,
 (SELECT wire FROM p5c2d_key) token_wire_id,
 (SELECT public_digest FROM p5c2d_key) token_public_digest,
 (SELECT jwk_digest FROM p5c2d_key) token_jwk_digest,
 decode(repeat('52',32),'hex') hwm_digest,
 (SELECT seal_epoch FROM public.kb_vault_control WHERE singleton=1) seal_epoch;
INSERT INTO public.kb_management_jwks_publication_permit
  VALUES(pg_backend_pid(),txid_current(),'stage');
INSERT INTO public.kb_management_jwks_publication_candidate(generation,candidate_id,phase,
 valid_from,valid_until,previous_manifest_sha256,jwks_bytes,jwks_sha256,payload_bytes,
 payload_sha256,envelope_bytes,envelope_sha256,manifest_sha256,signature,token_wire_id,
 token_public_digest,token_jwk_digest,manifest_wire_id,manifest_public_digest,
 publication_identity_digest,hwm1_attestation_digest,hwm2_attestation_digest,seal_epoch,
 created_at,finalized_at)
SELECT 1,candidate_id,'final',valid_from,valid_until,decode(repeat('00',32),'hex'),jwks_bytes,
 sha256(jwks_bytes),payload_bytes,sha256(payload_bytes),envelope_bytes,sha256(envelope_bytes),
 manifest_sha256,decode(repeat('53',64),'hex'),token_wire_id,token_public_digest,
 token_jwk_digest,'p5-jwks-root-test',decode(repeat('54',32),'hex'),
 decode(repeat('55',32),'hex'),decode(repeat('56',32),'hex'),hwm_digest,seal_epoch,now(),now()
 FROM p5c2d_pub;
DELETE FROM public.kb_management_jwks_publication_permit;
INSERT INTO public.kb_management_jwks_publication_permit
  VALUES(pg_backend_pid(),txid_current(),'finalize');
INSERT INTO public.kb_management_jwks_publication_generation(generation,candidate_id,valid_from,
 valid_until,previous_manifest_sha256,jwks_bytes,payload_bytes,envelope_bytes,envelope_sha256,
 manifest_sha256,jwks_sha256,payload_sha256,signature,manifest_wire_id,manifest_public_digest,
 token_wire_id,token_public_digest,token_jwk_digest,publication_identity_digest,
 hwm1_attestation_digest,hwm2_attestation_digest,seal_epoch,finalized_at)
SELECT 1,candidate_id,valid_from,valid_until,decode(repeat('00',32),'hex'),jwks_bytes,
 payload_bytes,envelope_bytes,sha256(envelope_bytes),manifest_sha256,sha256(jwks_bytes),
 sha256(payload_bytes),decode(repeat('53',64),'hex'),'p5-jwks-root-test',
 decode(repeat('54',32),'hex'),token_wire_id,token_public_digest,token_jwk_digest,
 decode(repeat('55',32),'hex'),decode(repeat('56',32),'hex'),hwm_digest,seal_epoch,now()
 FROM p5c2d_pub;
INSERT INTO public.kb_management_jwks_publication_registry
SELECT 1,1,candidate_id,manifest_sha256,sha256(envelope_bytes),hwm_digest,now() FROM p5c2d_pub;
DELETE FROM public.kb_management_jwks_publication_permit;

INSERT INTO public.kb_management_token_intent_namespace(correlation_id,jti,kind)
VALUES(repeat('6',64),repeat('7',64),'action');
INSERT INTO public.kb_management_action_intent(correlation_id,jti,kind,team_id,actor_identity,
 capability,target_server_id,request_sha256,token_issuer,audience,kid,issued_at,expires_at,
 installation_id,installation_generation,installation_enrollment_id,local_cert_issuer,
 local_cert_serial_norm,local_cert_fingerprint,target_enrollment_id,target_mgmt_issuer,
 target_mgmt_serial_norm,target_mgmt_fingerprint,revocation_generation)
SELECT repeat('6',64),repeat('7',64),'action',97522,'oidc:https%3A%25issuer:p5c2d-lead',
 'remote_writes','srv-p5c2d',repeat('8',64),'https://kb.p5c2d.test','srv-p5c2d',wire,
 floor(extract(epoch FROM clock_timestamp()))::BIGINT,
 floor(extract(epoch FROM clock_timestamp()))::BIGINT+60,repeat('3',32),1,97521,
 '/CN=p5c2d-local-ca','11',repeat('1',64),97522,'/CN=p5c2d-target-ca','22',
 repeat('2',64),(SELECT generation FROM public.kb_cert_revocation_generation WHERE singleton=1)
 FROM p5c2d_key;

-- These legacy certificate timestamps are timezone-less text.  A hostile
-- session timezone must not shift their authority window inside the definer.
SET LOCAL TimeZone='Pacific/Kiritimati';
SET LOCAL ROLE aimee_kb_token_authority_runtime;
DO $$ DECLARE r RECORD; BEGIN
 SELECT * INTO STRICT r FROM public.kb_management_token_authority_admit(repeat('6',64),repeat('7',64));
 IF NOT r.newly_admitted OR r.token_version<>2 OR r.publication_generation<>1 OR
    r.correlation_id<>repeat('6',64) OR octet_length(r.token_public_key)<>384 OR
    octet_length(r.wrapped_dek)<>40 OR octet_length(r.hwm_attestation)<>64 THEN
   RAISE EXCEPTION 'P5-C2d admitted row mismatch'; END IF;
 SELECT * INTO STRICT r FROM public.kb_management_token_authority_admit(repeat('6',64),repeat('7',64));
 IF r.newly_admitted THEN RAISE EXCEPTION 'P5-C2d exact replay admitted twice'; END IF;
 SELECT * INTO STRICT r FROM public.kb_management_token_authority_use(repeat('6',64),repeat('7',64));
 IF r.newly_admitted OR NOT public.kb_management_token_authority_finalize(
      repeat('6',64),repeat('7',64)) THEN RAISE EXCEPTION 'P5-C2d use/finalize failed'; END IF;
 SELECT * INTO STRICT r FROM public.kb_management_token_authority_readback(repeat('6',64),repeat('7',64));
 IF r.key_use_created_at_epoch IS NULL THEN RAISE EXCEPTION 'P5-C2d readback failed'; END IF;
END $$;
RESET ROLE;

DO $$ BEGIN
 IF (SELECT count(*) FROM public.kb_management_token_key_use_intent
      WHERE correlation_id=repeat('6',64) AND purpose='management.token.sign.v1')<>1 OR
    (SELECT count(*) FROM public.kb_audit_outbox WHERE action='vault.key_use'
      AND actor_principal='management-token-authority' AND detail NOT LIKE '%ciphertext%')<>1 THEN
   RAISE EXCEPTION 'P5-C2d WORM admission cardinality mismatch'; END IF;
END $$;

-- A fresh revocation generation invalidates the admitted tuple before another
-- protected use; no key-use or audit row is appended by the denial.
UPDATE public.kb_cert_revocation_generation SET generation=generation+1 WHERE singleton=1;
SET LOCAL ROLE aimee_kb_token_authority_runtime;
DO $$ BEGIN
 BEGIN
  PERFORM * FROM public.kb_management_token_authority_use(repeat('6',64),repeat('7',64));
  RAISE EXCEPTION 'P5-C2d stale revocation snapshot accepted';
 EXCEPTION WHEN serialization_failure THEN NULL; END;
END $$;
RESET ROLE;

ROLLBACK;
\echo 'P5-C2d token authority PostgreSQL 17 gate passed'
