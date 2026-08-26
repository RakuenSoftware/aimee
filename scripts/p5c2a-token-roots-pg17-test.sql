\set ON_ERROR_STOP on

-- P5-C2a focused PostgreSQL 17 authority/state-machine gate.  Run after
-- schema_roles.sql, schema.sql, and schema_grants.sql have been installed in a
-- throwaway database.  The whole fixture is rolled back.
BEGIN;

DO $$
DECLARE
  owner_oid OID;
  role_name TEXT;
  fn REGPROCEDURE;
BEGIN
  SELECT oid INTO owner_oid FROM pg_catalog.pg_roles WHERE rolname='aimee_kb_owner';
  IF owner_oid IS NULL OR NOT EXISTS (
      SELECT 1 FROM pg_catalog.pg_roles WHERE rolname='aimee_kb_token_roots_provision'
        AND NOT rolcanlogin AND NOT rolinherit AND NOT rolbypassrls AND NOT rolsuper
        AND NOT rolcreatedb AND NOT rolcreaterole AND NOT rolreplication) THEN
    RAISE EXCEPTION 'P5-C2a owner/provision role posture mismatch';
  END IF;

  IF (SELECT count(*) FROM pg_catalog.pg_proc p
       WHERE p.oid=ANY(ARRAY[
        'public.kb_management_token_root_bootstrap_resume(text,text)'::regprocedure,
        'public.kb_management_token_root_bootstrap_stage(text,text,text,text,bytea,bytea,bytea,bigint,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea)'::regprocedure,
        'public.kb_management_token_root_bootstrap_record_cas(text,text,bytea)'::regprocedure,
        'public.kb_management_token_root_bootstrap_finalize(text,text)'::regprocedure,
        'public.kb_management_jwks_publication_root_inspect()'::regprocedure,
        'public.kb_management_jwks_publication_root_bind(text,text,text,bytea,bytea)'::regprocedure])
       AND p.prosecdef AND p.proowner=owner_oid
       AND p.proconfig @> ARRAY['search_path=pg_catalog, pg_temp']::text[])<>6 THEN
    RAISE EXCEPTION 'P5-C2a functions are not fixed owner-hardened definers';
  END IF;

  FOREACH fn IN ARRAY ARRAY[
    'public.kb_management_token_root_bootstrap_resume(text,text)'::regprocedure,
    'public.kb_management_token_root_bootstrap_stage(text,text,text,text,bytea,bytea,bytea,bigint,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea)'::regprocedure,
    'public.kb_management_token_root_bootstrap_record_cas(text,text,bytea)'::regprocedure,
    'public.kb_management_token_root_bootstrap_finalize(text,text)'::regprocedure,
    'public.kb_management_jwks_publication_root_inspect()'::regprocedure,
    'public.kb_management_jwks_publication_root_bind(text,text,text,bytea,bytea)'::regprocedure]
  LOOP
    IF NOT has_function_privilege('aimee_kb_token_roots_provision',fn,'EXECUTE') OR EXISTS (
        SELECT 1 FROM pg_catalog.pg_proc p,
          LATERAL pg_catalog.aclexplode(COALESCE(p.proacl,pg_catalog.acldefault('f',p.proowner))) a
        WHERE p.oid=fn AND a.grantee=0 AND a.privilege_type='EXECUTE') THEN
      RAISE EXCEPTION 'P5-C2a provision/PUBLIC function ACL mismatch on %',fn;
    END IF;
    FOREACH role_name IN ARRAY ARRAY['aimee_kb_runtime','aimee_kb_status',
        'aimee_kb_status_definer','aimee_kb_status_login'] LOOP
      IF has_function_privilege(role_name,fn,'EXECUTE') THEN
        RAISE EXCEPTION 'online/status role % can execute %',role_name,fn;
      END IF;
    END LOOP;
  END LOOP;

  IF EXISTS (
      SELECT 1 FROM pg_catalog.pg_class c,
        LATERAL pg_catalog.aclexplode(COALESCE(c.relacl,pg_catalog.acldefault('r',c.relowner))) a
      WHERE c.oid=ANY(ARRAY['public.kb_management_token_root'::regclass,
        'public.kb_management_jwks_publication_root'::regclass,
        'public.kb_management_token_root_vault_permit'::regclass]) AND a.grantee=0) THEN
    RAISE EXCEPTION 'PUBLIC has direct P5-C2a registry authority';
  END IF;

  FOREACH role_name IN ARRAY ARRAY['aimee_kb_token_roots_provision',
      'aimee_kb_runtime','aimee_kb_status','aimee_kb_status_definer',
      'aimee_kb_status_login'] LOOP
    IF has_table_privilege(role_name,'public.kb_management_token_root','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_token_root','INSERT') OR
       has_table_privilege(role_name,'public.kb_management_token_root','UPDATE') OR
       has_table_privilege(role_name,'public.kb_management_token_root','DELETE') OR
       has_table_privilege(role_name,'public.kb_management_token_root','TRUNCATE') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_root','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_root','INSERT') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_root','UPDATE') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_root','DELETE') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_root','TRUNCATE') OR
       has_table_privilege(role_name,'public.kb_management_token_root_vault_permit','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_token_root_vault_permit','INSERT') OR
       has_table_privilege(role_name,'public.kb_management_token_root_vault_permit','DELETE') THEN
      RAISE EXCEPTION 'role % has direct P5-C2a registry authority',role_name;
    END IF;
  END LOOP;
END $$;

-- The reserved vault slots are unavailable through generic table paths even
-- before a registry row exists.  Moving an unrelated row into either reserved
-- slot is denied as well; bootstrap authority is not inferred from row shape.
DO $$
BEGIN
  BEGIN
    INSERT INTO public.org_vault_secret(principal,team_id,agent,cred,version,
      wrapped_dek,nonce,ciphertext,tag) VALUES('org:p5-token',NULL,'management','rs256',1,
      decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),decode('03','hex'),
      decode(repeat('04',16),'hex'));
    RAISE EXCEPTION 'generic token secret preseed accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN
    INSERT INTO public.org_vault_current(principal,agent,cred,version)
      VALUES('org:p5-token','management','rs256',1);
    RAISE EXCEPTION 'generic token current preseed accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN
    INSERT INTO public.org_vault_rotation(key_id,principal,team_id,agent,cred,
      from_version,to_version,state) VALUES('generic-preseed','org:p5-jwks-manifest',NULL,
      'management','ed25519',1,2,'staged');
    RAISE EXCEPTION 'generic manifest rotation preseed accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;

  INSERT INTO public.org_vault_secret(principal,team_id,agent,cred,version,
    wrapped_dek,nonce,ciphertext,tag) VALUES('org:p5c2a-unrelated',NULL,'management','other',1,
    decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),decode('13','hex'),
    decode(repeat('14',16),'hex'));
  INSERT INTO public.org_vault_current(principal,agent,cred,version)
    VALUES('org:p5c2a-unrelated','management','other',1);
  INSERT INTO public.org_vault_rotation(key_id,principal,team_id,agent,cred,
    from_version,to_version,state) VALUES('generic-update','org:p5c2a-unrelated',NULL,
    'management','other',1,2,'staged');
  BEGIN
    UPDATE public.org_vault_secret SET principal='org:p5-token',cred='rs256'
      WHERE principal='org:p5c2a-unrelated';
    RAISE EXCEPTION 'generic secret slot substitution accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN
    UPDATE public.org_vault_current SET principal='org:p5-jwks-manifest',cred='ed25519'
      WHERE principal='org:p5c2a-unrelated';
    RAISE EXCEPTION 'generic current slot substitution accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN
    UPDATE public.org_vault_rotation SET principal='org:p5-token',cred='rs256'
      WHERE key_id='generic-update';
    RAISE EXCEPTION 'generic rotation slot substitution accepted';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  DELETE FROM public.org_vault_rotation WHERE key_id='generic-update';
  DELETE FROM public.org_vault_current WHERE principal='org:p5c2a-unrelated';
  DELETE FROM public.org_vault_secret WHERE principal='org:p5c2a-unrelated';
END $$;

-- Reproduce the production canonical AAD and envelope-digest constructions so
-- the SQL gate does not bless opaque/random digest placeholders.
CREATE FUNCTION pg_temp.p5c2a_aad(p_principal TEXT,p_cred TEXT,p_version BIGINT)
RETURNS BYTEA LANGUAGE sql IMMUTABLE AS $$
  SELECT convert_to('kb.vault.envelope','UTF8')||decode('00','hex')||decode('02','hex')
    ||int4send(octet_length(convert_to(p_principal,'UTF8')))||convert_to(p_principal,'UTF8')
    ||int4send(octet_length(convert_to('management','UTF8')))||convert_to('management','UTF8')
    ||int4send(octet_length(convert_to(p_cred,'UTF8')))||convert_to(p_cred,'UTF8')
    ||int8send(p_version)
$$;

CREATE FUNCTION pg_temp.p5c2a_envelope_digest(p_kind TEXT,p_version BIGINT,p_wrapped BYTEA,
  p_nonce BYTEA,p_ciphertext BYTEA,p_tag BYTEA) RETURNS BYTEA LANGUAGE sql IMMUTABLE AS $$
  SELECT sha256(convert_to('aimee.management.root.envelope.v1'||chr(10),'UTF8')
    ||CASE p_kind WHEN 'token' THEN decode('01','hex') ELSE decode('02','hex') END
    ||int8send(p_version)
    ||int4send(octet_length(a.aad))||a.aad
    ||int4send(octet_length(p_wrapped))||p_wrapped
    ||int4send(octet_length(p_nonce))||p_nonce
    ||int4send(octet_length(p_ciphertext))||p_ciphertext
    ||int4send(octet_length(p_tag))||p_tag)
  FROM (SELECT pg_temp.p5c2a_aad(
    CASE p_kind WHEN 'token' THEN 'org:p5-token' ELSE 'org:p5-jwks-manifest' END,
    CASE p_kind WHEN 'token' THEN 'rs256' ELSE 'ed25519' END,p_version) aad) a
$$;

CREATE TEMP TABLE p5c2a_fixture(
  kind TEXT PRIMARY KEY,custody TEXT NOT NULL,bootstrap TEXT NOT NULL,wire TEXT NOT NULL,
  public_key BYTEA NOT NULL,public_digest BYTEA NOT NULL,jwk_digest BYTEA NOT NULL,
  hwm1 BYTEA NOT NULL,hwm2 BYTEA NOT NULL,
  v1_wrapped BYTEA NOT NULL,v1_nonce BYTEA NOT NULL,v1_ciphertext BYTEA NOT NULL,v1_tag BYTEA NOT NULL,
  v2_wrapped BYTEA NOT NULL,v2_nonce BYTEA NOT NULL,v2_ciphertext BYTEA NOT NULL,v2_tag BYTEA NOT NULL,
  v1_digest BYTEA NOT NULL,v2_digest BYTEA NOT NULL);

WITH raw(kind,custody,public_key,hwm1,hwm2,v1_wrapped,v1_nonce,v1_ciphertext,v1_tag,
         v2_wrapped,v2_nonce,v2_ciphertext,v2_tag) AS (VALUES
  ('token','p5c2a-token-custody',decode('01'||repeat('11',383),'hex'),decode(repeat('a1',64),'hex'),
    decode(repeat('a2',64),'hex'),decode(repeat('21',40),'hex'),decode(repeat('22',12),'hex'),
    decode(repeat('23',64),'hex'),decode(repeat('24',16),'hex'),decode(repeat('31',40),'hex'),
    decode(repeat('32',12),'hex'),decode(repeat('33',64),'hex'),decode(repeat('34',16),'hex')),
  ('manifest','p5c2a-manifest-custody',decode(repeat('41',32),'hex'),decode(repeat('b1',64),'hex'),
    decode(repeat('b2',64),'hex'),decode(repeat('51',40),'hex'),decode(repeat('52',12),'hex'),
    decode(repeat('53',32),'hex'),decode(repeat('54',16),'hex'),decode(repeat('61',40),'hex'),
    decode(repeat('62',12),'hex'),decode(repeat('63',32),'hex'),decode(repeat('64',16),'hex'))),
bindings AS (
  SELECT r.*,CASE r.kind
    WHEN 'token' THEN 'p5-token-v1-'||substr(encode(sha256(
      convert_to('aimee.p5.token.public.v1'||chr(10),'UTF8')||int4send(384)||r.public_key
      ||int4send(3)||decode('010001','hex')),'hex'),1,32)
    ELSE 'p5-jwks-root-v1-'||substr(encode(sha256(r.public_key),'hex'),1,32) END wire
  FROM raw r), digests AS (
  SELECT b.*,sha256(b.public_key) public_digest,
    CASE b.kind WHEN 'token' THEN sha256(convert_to('{"kty":"RSA","kid":"'||b.wire
      ||'","use":"sig","alg":"RS256","n":"'
      ||rtrim(replace(replace(replace(encode(b.public_key,'base64'),chr(10),''),'+','-'),'/','_'),'=')
      ||'","e":"AQAB"}','UTF8')) ELSE decode(repeat('00',32),'hex') END jwk_digest
  FROM bindings b)
INSERT INTO p5c2a_fixture
SELECT kind,custody,encode(sha256(convert_to(CASE kind WHEN 'token'
    THEN 'aimee-p5-token-root-bootstrap-v1|' ELSE 'aimee-p5-jwks-manifest-root-bootstrap-v1|' END
    ||custody,'UTF8')),'hex'),wire,public_key,public_digest,jwk_digest,hwm1,hwm2,
  v1_wrapped,v1_nonce,v1_ciphertext,v1_tag,v2_wrapped,v2_nonce,v2_ciphertext,v2_tag,
  pg_temp.p5c2a_envelope_digest(kind,1,v1_wrapped,v1_nonce,v1_ciphertext,v1_tag),
  pg_temp.p5c2a_envelope_digest(kind,2,v2_wrapped,v2_nonce,v2_ciphertext,v2_tag)
FROM digests;

GRANT SELECT ON p5c2a_fixture TO aimee_kb_token_roots_provision;

CREATE FUNCTION pg_temp.p5c2a_stage(p_kind TEXT) RETURNS VOID LANGUAGE plpgsql SECURITY DEFINER
SET search_path=pg_catalog,public,pg_temp AS $$
DECLARE f p5c2a_fixture%ROWTYPE; e BIGINT;
BEGIN
  SELECT * INTO STRICT f FROM p5c2a_fixture WHERE kind=p_kind;
  SELECT seal_epoch INTO e FROM public.kb_vault_control WHERE singleton=1;
  PERFORM public.kb_management_token_root_bootstrap_stage(f.kind,f.bootstrap,f.custody,f.wire,
    f.public_key,f.public_digest,f.jwk_digest,e,f.hwm1,f.v1_wrapped,f.v1_nonce,
    f.v1_ciphertext,f.v1_tag,f.v2_wrapped,f.v2_nonce,f.v2_ciphertext,f.v2_tag,
    f.v1_digest,f.v2_digest);
END $$;

CREATE FUNCTION pg_temp.p5c2a_audit_count(p_action TEXT,p_subject TEXT) RETURNS BIGINT
LANGUAGE sql STABLE SECURITY DEFINER SET search_path=pg_catalog,pg_temp AS $$
  SELECT count(*) FROM public.kb_audit_outbox WHERE action=p_action AND subject=p_subject
$$;

CREATE FUNCTION pg_temp.p5c2a_epoch() RETURNS BIGINT LANGUAGE sql STABLE SECURITY DEFINER
SET search_path=pg_catalog,pg_temp AS $$
  SELECT seal_epoch FROM public.kb_vault_control WHERE singleton=1
$$;

-- NULL/non-canonical inputs fail before writing any root, vault, or WORM row.
SET LOCAL ROLE aimee_kb_token_roots_provision;
DO $$
DECLARE f p5c2a_fixture%ROWTYPE;
BEGIN
  BEGIN PERFORM * FROM public.kb_management_token_root_bootstrap_resume('token',NULL);
    RAISE EXCEPTION 'NULL custody accepted'; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN PERFORM * FROM public.kb_management_token_root_bootstrap_resume(NULL,'x');
    RAISE EXCEPTION 'NULL kind accepted'; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  SELECT * INTO STRICT f FROM p5c2a_fixture WHERE kind='token';
  BEGIN
    PERFORM public.kb_management_token_root_bootstrap_stage(f.kind,f.bootstrap,f.custody,
      'p5-token-v1-'||repeat('0',32),f.public_key,f.public_digest,f.jwk_digest,
      pg_temp.p5c2a_epoch(),f.hwm1,
      f.v1_wrapped,f.v1_nonce,f.v1_ciphertext,f.v1_tag,f.v2_wrapped,f.v2_nonce,
      f.v2_ciphertext,f.v2_tag,f.v1_digest,f.v2_digest);
    RAISE EXCEPTION 'non-canonical wire id accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM public.kb_management_jwks_publication_root_bind(NULL,'helper','domain',
      decode(repeat('71',32),'hex'),decode(repeat('72',64),'hex'));
    RAISE EXCEPTION 'NULL publication custody accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
RESET ROLE;

DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_management_token_root) OR
     EXISTS(SELECT 1 FROM public.kb_management_jwks_publication_root) OR
     EXISTS(SELECT 1 FROM public.org_vault_secret WHERE principal LIKE 'org:p5-%') THEN
    RAISE EXCEPTION 'negative input left P5-C2a state';
  END IF;
END $$;

-- Token EMPTY -> STAGED -> CAS_DONE -> FINAL, including exact phase resumes.
SET LOCAL ROLE aimee_kb_token_roots_provision;
DO $$ DECLARE f p5c2a_fixture%ROWTYPE; r RECORD; BEGIN
  SELECT * INTO STRICT f FROM p5c2a_fixture WHERE kind='token';
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'empty' OR r.bootstrap_id<>f.bootstrap OR r.seal_epoch<1 THEN
    RAISE EXCEPTION 'token EMPTY mismatch'; END IF;
  PERFORM pg_temp.p5c2a_stage('token');
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'staged' OR r.wire_id<>f.wire OR r.public_key<>f.public_key OR
     r.public_digest<>f.public_digest OR r.jwk_digest<>f.jwk_digest OR
     r.v1_envelope_digest<>f.v1_digest OR r.v2_envelope_digest<>f.v2_digest OR
     r.hwm1_attestation<>f.hwm1 OR r.hwm2_attestation IS NOT NULL THEN
    RAISE EXCEPTION 'token STAGED mismatch'; END IF;
  PERFORM public.kb_management_token_root_bootstrap_record_cas(f.kind,f.bootstrap,f.hwm2);
  PERFORM public.kb_management_token_root_bootstrap_record_cas(f.kind,f.bootstrap,f.hwm2);
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'cas_done' OR r.hwm2_attestation<>f.hwm2 THEN
    RAISE EXCEPTION 'token CAS_DONE mismatch'; END IF;
  BEGIN PERFORM public.kb_management_token_root_bootstrap_record_cas(
      f.kind,f.bootstrap,decode(repeat('ff',64),'hex'));
    RAISE EXCEPTION 'token changed CAS replay accepted'; EXCEPTION WHEN unique_violation THEN NULL; END;
  PERFORM public.kb_management_token_root_bootstrap_finalize(f.kind,f.bootstrap);
  PERFORM public.kb_management_token_root_bootstrap_finalize(f.kind,f.bootstrap);
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'final' OR r.hwm2_attestation<>f.hwm2 THEN RAISE EXCEPTION 'token FINAL mismatch'; END IF;
END $$;
RESET ROLE;

-- A failed stage WORM append rolls back the manifest registry, both envelopes,
-- current pointer, rotation, and audit row as a single transaction.
CREATE FUNCTION pg_temp.p5c2a_fail_manifest_stage() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.action='management.token_root.bootstrap.stage' AND NEW.subject='p5c2a-manifest-custody'
    THEN RAISE EXCEPTION 'injected P5-C2a WORM failure'; END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER p5c2a_fail_manifest_stage BEFORE INSERT ON public.kb_audit_outbox
  FOR EACH ROW EXECUTE FUNCTION pg_temp.p5c2a_fail_manifest_stage();
SET LOCAL ROLE aimee_kb_token_roots_provision;
DO $$ BEGIN
  BEGIN PERFORM pg_temp.p5c2a_stage('manifest'); RAISE EXCEPTION 'WORM failure accepted';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM<>'injected P5-C2a WORM failure' THEN RAISE; END IF;
  END;
END $$;
RESET ROLE;
DROP TRIGGER p5c2a_fail_manifest_stage ON public.kb_audit_outbox;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_management_token_root WHERE root_kind='manifest') OR
     EXISTS(SELECT 1 FROM public.org_vault_secret WHERE principal='org:p5-jwks-manifest') OR
     EXISTS(SELECT 1 FROM public.org_vault_current WHERE principal='org:p5-jwks-manifest') OR
     EXISTS(SELECT 1 FROM public.org_vault_rotation WHERE principal='org:p5-jwks-manifest') OR
     EXISTS(SELECT 1 FROM public.kb_audit_outbox WHERE action='management.token_root.bootstrap.stage'
       AND subject='p5c2a-manifest-custody') THEN
    RAISE EXCEPTION 'manifest stage/WORM rollback was not atomic';
  END IF;
END $$;

-- Manifest walks the same state machine after the injected failure.
SET LOCAL ROLE aimee_kb_token_roots_provision;
DO $$ DECLARE f p5c2a_fixture%ROWTYPE; r RECORD; BEGIN
  SELECT * INTO STRICT f FROM p5c2a_fixture WHERE kind='manifest';
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'empty' OR r.bootstrap_id<>f.bootstrap THEN RAISE EXCEPTION 'manifest EMPTY mismatch'; END IF;
  PERFORM pg_temp.p5c2a_stage('manifest');
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'staged' OR r.wire_id<>f.wire OR r.public_key<>f.public_key OR
     r.public_digest<>f.public_digest OR r.jwk_digest<>decode(repeat('00',32),'hex') OR
     r.v1_envelope_digest<>f.v1_digest OR r.v2_envelope_digest<>f.v2_digest THEN
    RAISE EXCEPTION 'manifest STAGED mismatch'; END IF;
  PERFORM public.kb_management_token_root_bootstrap_record_cas(f.kind,f.bootstrap,f.hwm2);
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'cas_done' OR r.hwm2_attestation<>f.hwm2 THEN RAISE EXCEPTION 'manifest CAS_DONE mismatch'; END IF;
  PERFORM public.kb_management_token_root_bootstrap_finalize(f.kind,f.bootstrap);
  PERFORM public.kb_management_token_root_bootstrap_finalize(f.kind,f.bootstrap);
  SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
  IF r.phase<>'final' OR r.hwm2_attestation<>f.hwm2 THEN RAISE EXCEPTION 'manifest FINAL mismatch'; END IF;
END $$;
RESET ROLE;

-- Publication-HWM root: exact bind converges without a second WORM row; any
-- binding or attestation change is a hard replay mismatch.
SET LOCAL ROLE aimee_kb_token_roots_provision;
DO $$ DECLARE r RECORD; n BIGINT; BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_management_jwks_publication_root_inspect()) THEN
    RAISE EXCEPTION 'publication root was not empty'; END IF;
  PERFORM public.kb_management_jwks_publication_root_bind('p5c2a-publication-custody',
    'aimee-hwm-helper','aimee.p5.jwks.hwm.v1',decode(repeat('71',32),'hex'),
    decode(repeat('72',64),'hex'));
  SELECT * INTO r FROM public.kb_management_jwks_publication_root_inspect();
  IF r.custody_key_id<>'p5c2a-publication-custody' OR r.helper<>'aimee-hwm-helper' OR
     r.verifier_domain<>'aimee.p5.jwks.hwm.v1' OR r.identity_digest<>decode(repeat('71',32),'hex') OR
     r.hwm1_attestation_digest<>sha256(decode(repeat('72',64),'hex')) THEN
    RAISE EXCEPTION 'publication root binding mismatch'; END IF;
  n:=pg_temp.p5c2a_audit_count('management.jwks.publication_root.bind',
    'p5c2a-publication-custody');
  PERFORM public.kb_management_jwks_publication_root_bind('p5c2a-publication-custody',
    'aimee-hwm-helper','aimee.p5.jwks.hwm.v1',decode(repeat('71',32),'hex'),
    decode(repeat('72',64),'hex'));
  IF pg_temp.p5c2a_audit_count('management.jwks.publication_root.bind',
      'p5c2a-publication-custody')<>n THEN
    RAISE EXCEPTION 'publication exact replay duplicated WORM row'; END IF;
  BEGIN PERFORM public.kb_management_jwks_publication_root_bind('p5c2a-publication-custody',
      'changed-helper','aimee.p5.jwks.hwm.v1',decode(repeat('71',32),'hex'),decode(repeat('72',64),'hex'));
    RAISE EXCEPTION 'publication binding mismatch replay accepted'; EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN PERFORM public.kb_management_jwks_publication_root_bind('p5c2a-publication-custody',
      'aimee-hwm-helper','aimee.p5.jwks.hwm.v1',decode(repeat('71',32),'hex'),decode(repeat('73',64),'hex'));
    RAISE EXCEPTION 'publication HWM replay mismatch accepted'; EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;
RESET ROLE;

-- Runtime and status paths get neither registry reads nor facade calls. Runtime's
-- generic tenant vault SELECT grant still resolves the platform rows to zero.
DO $$ DECLARE role_name TEXT; n BIGINT; BEGIN
  FOREACH role_name IN ARRAY ARRAY['aimee_kb_runtime','aimee_kb_status','aimee_kb_status_login'] LOOP
    EXECUTE format('SET LOCAL ROLE %I',role_name);
    BEGIN PERFORM * FROM public.kb_management_token_root;
      RAISE EXCEPTION '% read token roots',role_name; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
    BEGIN PERFORM * FROM public.kb_management_jwks_publication_root;
      RAISE EXCEPTION '% read publication root',role_name; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
    IF role_name='aimee_kb_runtime' THEN
      SELECT count(*) INTO n FROM public.org_vault_secret WHERE principal='org:p5-token';
      IF n<>0 THEN RAISE EXCEPTION 'runtime observed root ciphertext'; END IF;
    ELSE
      BEGIN PERFORM * FROM public.org_vault_secret WHERE principal='org:p5-token';
        RAISE EXCEPTION '% read root ciphertext',role_name; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
    END IF;
    BEGIN PERFORM * FROM public.kb_management_token_root_bootstrap_resume('token','p5c2a-token-custody');
      RAISE EXCEPTION '% invoked provision function',role_name; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
    RESET ROLE;
  END LOOP;
END $$;

-- Once FINAL, even the owning role cannot rewrite/delete/truncate the registry
-- or any special-slot secret/current/rotation row, nor add a third version/row.
DO $$ BEGIN
  BEGIN UPDATE public.kb_management_token_root SET wire_id=wire_id||'x' WHERE root_kind='token';
    RAISE EXCEPTION 'FINAL root update accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN DELETE FROM public.kb_management_token_root WHERE root_kind='token';
    RAISE EXCEPTION 'FINAL root delete accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN TRUNCATE public.kb_management_token_root;
    RAISE EXCEPTION 'FINAL root truncate accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN UPDATE public.kb_management_jwks_publication_root SET helper='x';
    RAISE EXCEPTION 'publication update accepted'; EXCEPTION WHEN raise_exception THEN
      IF SQLERRM='publication update accepted' THEN RAISE; END IF; END;
  BEGIN DELETE FROM public.kb_management_jwks_publication_root;
    RAISE EXCEPTION 'publication delete accepted'; EXCEPTION WHEN raise_exception THEN
      IF SQLERRM='publication delete accepted' THEN RAISE; END IF; END;
  BEGIN TRUNCATE public.kb_management_jwks_publication_root;
    RAISE EXCEPTION 'publication truncate accepted'; EXCEPTION WHEN raise_exception THEN
      IF SQLERRM='publication truncate accepted' THEN RAISE; END IF; END;

  BEGIN INSERT INTO public.org_vault_secret(principal,team_id,agent,cred,version,wrapped_dek,nonce,ciphertext,tag)
      VALUES('org:p5-token',NULL,'management','rs256',3,decode(repeat('81',40),'hex'),
        decode(repeat('82',12),'hex'),decode(repeat('83',32),'hex'),decode(repeat('84',16),'hex'));
    RAISE EXCEPTION 'special secret insert accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN UPDATE public.org_vault_secret SET ciphertext=decode(repeat('85',64),'hex')
      WHERE principal='org:p5-token' AND version=2;
    RAISE EXCEPTION 'special secret update accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN DELETE FROM public.org_vault_secret WHERE principal='org:p5-token' AND version=1;
    RAISE EXCEPTION 'special secret delete accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN TRUNCATE public.org_vault_secret,public.org_vault_current,public.org_vault_rotation;
    RAISE EXCEPTION 'special secret truncate accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;

  BEGIN INSERT INTO public.org_vault_current VALUES('org:p5-token','management','rs256',2,public.pg_now_text());
    RAISE EXCEPTION 'special current insert accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN UPDATE public.org_vault_current SET updated_at='changed' WHERE principal='org:p5-token';
    RAISE EXCEPTION 'special current update accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN DELETE FROM public.org_vault_current WHERE principal='org:p5-token';
    RAISE EXCEPTION 'special current delete accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN TRUNCATE public.org_vault_current;
    RAISE EXCEPTION 'special current truncate accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;

  BEGIN INSERT INTO public.org_vault_rotation(key_id,principal,team_id,agent,cred,from_version,to_version,state)
      VALUES('p5c2a-extra','org:p5-token',NULL,'management','rs256',2,3,'staged');
    RAISE EXCEPTION 'special rotation insert accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN UPDATE public.org_vault_rotation SET updated_at='changed' WHERE principal='org:p5-token';
    RAISE EXCEPTION 'special rotation update accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN DELETE FROM public.org_vault_rotation WHERE principal='org:p5-token';
    RAISE EXCEPTION 'special rotation delete accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
  BEGIN TRUNCATE public.org_vault_rotation;
    RAISE EXCEPTION 'special rotation truncate accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
END $$;

-- FINAL state is epoch-independent: after a completed seal cycle changes the
-- epoch, resume returns the exact root instead of treating old initial epoch as
-- a partially-staged conflict.
UPDATE public.kb_vault_control SET seal_epoch=seal_epoch+1,updated_at=public.pg_now_text()
  WHERE singleton=1;
SET LOCAL ROLE aimee_kb_token_roots_provision;
DO $$ DECLARE f p5c2a_fixture%ROWTYPE; r RECORD; current_epoch BIGINT; BEGIN
  current_epoch:=pg_temp.p5c2a_epoch();
  FOR f IN SELECT * FROM p5c2a_fixture ORDER BY kind LOOP
    SELECT * INTO r FROM public.kb_management_token_root_bootstrap_resume(f.kind,f.custody);
    IF r.phase<>'final' OR r.seal_epoch<>current_epoch OR r.public_key<>f.public_key OR
       r.hwm2_attestation<>f.hwm2 THEN RAISE EXCEPTION 'post-epoch FINAL resume mismatch for %',f.kind; END IF;
  END LOOP;
END $$;
RESET ROLE;

DO $$ BEGIN
  IF (SELECT count(*) FROM public.kb_management_token_root)<>2 OR
     (SELECT count(*) FROM public.kb_management_token_root WHERE enabled AND current_version=2)<>2 OR
     (SELECT count(*) FROM public.org_vault_secret WHERE principal IN
       ('org:p5-token','org:p5-jwks-manifest'))<>4 OR
     (SELECT count(*) FROM public.org_vault_current WHERE principal IN
       ('org:p5-token','org:p5-jwks-manifest') AND version=2)<>2 OR
     (SELECT count(*) FROM public.org_vault_rotation WHERE principal IN
       ('org:p5-token','org:p5-jwks-manifest') AND state='activated')<>2 OR
     (SELECT count(*) FROM public.kb_management_jwks_publication_root)<>1 OR
     (SELECT count(*) FROM public.kb_audit_outbox
       WHERE action='management.token_root.bootstrap.stage'
         AND subject IN ('p5c2a-token-custody','p5c2a-manifest-custody'))<>2 OR
     (SELECT count(*) FROM public.kb_audit_outbox
       WHERE action='management.token_root.bootstrap.activate'
         AND subject IN ('p5c2a-token-custody','p5c2a-manifest-custody'))<>2 OR
     (SELECT count(*) FROM public.kb_audit_outbox
       WHERE action='management.jwks.publication_root.bind'
         AND subject='p5c2a-publication-custody')<>1 THEN
    RAISE EXCEPTION 'P5-C2a final row/WORM cardinality mismatch';
  END IF;
END $$;

ROLLBACK;
\echo 'P5-C2a token/JWKS roots PostgreSQL 17 gate passed'
