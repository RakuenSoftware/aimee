\set ON_ERROR_STOP on
BEGIN;

DO $$
DECLARE fn REGPROCEDURE; role_name TEXT; owner_oid OID; hardened BIGINT:=0;
BEGIN
  SELECT oid INTO owner_oid FROM pg_roles WHERE rolname='aimee_kb_owner';
  IF NOT EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_jwks_publish'
    AND NOT rolcanlogin AND NOT rolinherit AND NOT rolbypassrls AND NOT rolsuper
    AND NOT rolcreatedb AND NOT rolcreaterole AND NOT rolreplication) THEN
    RAISE EXCEPTION 'P5-C2b publisher role posture mismatch';
  END IF;
  FOREACH fn IN ARRAY ARRAY[
    'public.kb_management_jwks_publication_roots()'::regprocedure,
    'public.kb_management_jwks_publication_inspect()'::regprocedure,
    'public.kb_management_jwks_manifest_key_admit(text,bigint,text,text,text,bytea,bytea)'::regprocedure,
    'public.kb_management_jwks_publication_stage(bigint,text,bigint,bigint,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,text,bytea,bytea,text,bytea,bytea,bytea,bigint)'::regprocedure,
    'public.kb_management_jwks_publication_record_cas(bigint,text,bytea)'::regprocedure,
    'public.kb_management_jwks_publication_finalize(bigint,text)'::regprocedure,
    'public.kb_management_jwks_publication_final()'::regprocedure]
  LOOP
    IF NOT has_function_privilege('aimee_kb_jwks_publish',fn,'EXECUTE') OR EXISTS(
      SELECT 1 FROM pg_proc p,LATERAL aclexplode(COALESCE(p.proacl,acldefault('f',p.proowner))) a
       WHERE p.oid=fn AND a.grantee=0 AND a.privilege_type='EXECUTE') THEN
      RAISE EXCEPTION 'P5-C2b facade ACL mismatch: %',fn;
    END IF;
    IF EXISTS(SELECT 1 FROM pg_proc p WHERE p.oid=fn AND p.prosecdef AND
      p.proowner=owner_oid AND p.provolatile='v' AND
      p.proconfig @> ARRAY['search_path=pg_catalog, pg_temp']::TEXT[]) THEN
      hardened:=hardened+1;
    END IF;
    FOREACH role_name IN ARRAY ARRAY['aimee_kb_runtime','aimee_kb_status',
      'aimee_kb_token_roots_provision'] LOOP
      IF has_function_privilege(role_name,fn,'EXECUTE') THEN
        RAISE EXCEPTION 'P5-C2b forbidden role % executes %',role_name,fn;
      END IF;
    END LOOP;
  END LOOP;
  IF hardened<>7 THEN RAISE EXCEPTION 'P5-C2b facade owner/security posture mismatch'; END IF;
  IF has_function_privilege('aimee_kb_jwks_publish',
      'public.kb_management_token_root_bootstrap_resume(text,text)','EXECUTE') OR
     has_table_privilege('aimee_kb_jwks_publish','public.org_vault_secret','SELECT') OR
     has_table_privilege('aimee_kb_jwks_publish','public.kb_management_token_root','SELECT') THEN
    RAISE EXCEPTION 'P5-C2b publisher escaped fixed facade';
  END IF;
  FOREACH role_name IN ARRAY ARRAY['aimee_kb_jwks_publish','aimee_kb_runtime',
    'aimee_kb_status','aimee_kb_token_roots_provision'] LOOP
    IF has_table_privilege(role_name,'public.kb_management_jwks_publication_candidate','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_generation','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_registry','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_jwks_manifest_key_use_intent','SELECT') OR
       has_table_privilege(role_name,'public.kb_management_jwks_publication_permit','INSERT') THEN
      RAISE EXCEPTION 'P5-C2b direct table ACL for %',role_name;
    END IF;
  END LOOP;
END $$;

-- Earlier full-gate slices intentionally exercise sealed maintenance states.
-- This fixture begins from the post-operator-unseal state required by C2b.
UPDATE kb_vault_control
   SET sealed=false,maintenance_kind='',maintenance_id='',updated_at=pg_now_text()
 WHERE singleton=1;

-- Seed exact C2a-final roots and manifest v2 custody.  The C2a gate separately
-- proves its bootstrap; this fixture isolates the C2b authority transitions.
INSERT INTO kb_management_token_root_vault_permit VALUES
 (pg_backend_pid(),txid_current(),'manifest');
INSERT INTO org_vault_secret(principal,team_id,agent,cred,version,wrapped_dek,nonce,ciphertext,tag,hwm_attestation)
VALUES
 ('org:p5-jwks-manifest',NULL,'management','ed25519',1,decode(repeat('11',40),'hex'),
  decode(repeat('12',12),'hex'),decode(repeat('13',32),'hex'),decode(repeat('14',16),'hex'),decode(repeat('15',64),'hex')),
 ('org:p5-jwks-manifest',NULL,'management','ed25519',2,decode(repeat('21',40),'hex'),
  decode(repeat('22',12),'hex'),decode(repeat('23',32),'hex'),decode(repeat('24',16),'hex'),decode(repeat('25',64),'hex'));
INSERT INTO org_vault_current VALUES('org:p5-jwks-manifest','management','ed25519',2,pg_now_text());
INSERT INTO org_vault_rotation(key_id,principal,team_id,agent,cred,from_version,to_version,state,hwm_attestation)
 VALUES('p5c2b-manifest-custody','org:p5-jwks-manifest',NULL,'management','ed25519',1,2,'activated',decode(repeat('25',64),'hex'));
DELETE FROM kb_management_token_root_vault_permit;

WITH k AS (SELECT decode('01'||repeat('31',383),'hex') public_key), b AS (
 SELECT public_key,'p5-token-v1-'||substr(encode(sha256(convert_to(
  'aimee.p5.token.public.v1'||chr(10),'UTF8')||int4send(384)||public_key||
  int4send(3)||decode('010001','hex')),'hex'),1,32) wire FROM k)
INSERT INTO kb_management_token_root(root_kind,bootstrap_id,custody_key_id,wire_id,public_key,
 public_exponent,public_digest,jwk_digest,current_version,initial_seal_epoch,
 v1_envelope_digest,v2_envelope_digest,hwm2_attestation_digest,enabled)
SELECT 'token',repeat('a',64),'p5c2b-token-custody',wire,public_key,decode('010001','hex'),sha256(public_key),
 sha256(convert_to('{"kty":"RSA","kid":"'||wire||'","use":"sig","alg":"RS256","n":"'||
 rtrim(replace(replace(replace(encode(public_key,'base64'),chr(10),''),'+','-'),'/','_'),'=')||
 '","e":"AQAB"}','UTF8')),2,1,decode(repeat('41',32),'hex'),decode(repeat('42',32),'hex'),
 decode(repeat('43',32),'hex'),true FROM b;

WITH k AS (SELECT decode(repeat('51',32),'hex') public_key)
INSERT INTO kb_management_token_root(root_kind,bootstrap_id,custody_key_id,wire_id,public_key,
 public_exponent,public_digest,jwk_digest,current_version,initial_seal_epoch,
 v1_envelope_digest,v2_envelope_digest,hwm2_attestation_digest,enabled)
SELECT 'manifest',repeat('b',64),'p5c2b-manifest-custody',
 'p5-jwks-root-v1-'||substr(encode(sha256(public_key),'hex'),1,32),public_key,''::bytea,
 sha256(public_key),decode(repeat('00',32),'hex'),2,1,decode(repeat('52',32),'hex'),
 decode(repeat('53',32),'hex'),sha256(decode(repeat('25',64),'hex')),true FROM k;
INSERT INTO kb_management_jwks_publication_root VALUES
 (1,'p5c2b-publication-custody','hwm-helper','aimee.p5.jwks.hwm.v1',
  decode(repeat('61',32),'hex'),decode(repeat('62',64),'hex'),
  sha256(decode(repeat('62',64),'hex')),now());

CREATE FUNCTION pg_temp.p5c2b_use_id(p_candidate TEXT,p_payload BYTEA,p_epoch BIGINT)
RETURNS TEXT LANGUAGE sql IMMUTABLE AS $$
 SELECT encode(sha256(convert_to('aimee.management.jwks.manifest.use.v1'||chr(10),'UTF8')||
  int8send(1)||int8send(p_epoch)||convert_to(p_candidate,'UTF8')||sha256(p_payload)),'hex')
$$;
CREATE FUNCTION pg_temp.p5c2b_bump_epoch() RETURNS VOID LANGUAGE plpgsql SECURITY DEFINER
SET search_path=pg_catalog,public,pg_temp AS $$
BEGIN UPDATE kb_vault_control SET seal_epoch=seal_epoch+1,updated_at=pg_now_text() WHERE singleton=1; END $$;

CREATE TEMP TABLE p5c2b_fixture AS WITH x AS (SELECT
  repeat('c',64)::TEXT candidate_id,
  convert_to('{"format_version":1,"generation":1}','UTF8') payload)
SELECT x.candidate_id,pg_temp.p5c2b_use_id(x.candidate_id,x.payload,
  (SELECT seal_epoch FROM kb_vault_control WHERE singleton=1)) use_id,
  1700000000::BIGINT valid_from,1700000300::BIGINT valid_until,
  convert_to('{"keys":[]}','UTF8') jwks,x.payload,
  convert_to('{"payload":{}}','UTF8') envelope,
  decode(repeat('71',32),'hex') manifest_sha,decode(repeat('72',64),'hex') signature,
  decode(repeat('62',64),'hex') hwm1,decode(repeat('73',64),'hex') hwm2 FROM x;
GRANT SELECT ON p5c2b_fixture TO aimee_kb_jwks_publish;

CREATE FUNCTION pg_temp.p5c2b_stage(p_generation BIGINT,p_jwks BYTEA DEFAULT NULL,
 p_payload BYTEA DEFAULT NULL,p_envelope BYTEA DEFAULT NULL) RETURNS VOID
LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,public,pg_temp AS $$
DECLARE f p5c2b_fixture%ROWTYPE; t kb_management_token_root%ROWTYPE;
 m kb_management_token_root%ROWTYPE; p kb_management_jwks_publication_root%ROWTYPE;
BEGIN
 SELECT * INTO STRICT f FROM p5c2b_fixture; SELECT * INTO STRICT t FROM kb_management_token_root WHERE root_kind='token';
 SELECT * INTO STRICT m FROM kb_management_token_root WHERE root_kind='manifest';
 SELECT * INTO STRICT p FROM kb_management_jwks_publication_root;
 PERFORM kb_management_jwks_publication_stage(p_generation,f.candidate_id,f.valid_from,f.valid_until,
  decode(repeat('00',32),'hex'),COALESCE(p_jwks,f.jwks),sha256(COALESCE(p_jwks,f.jwks)),
  COALESCE(p_payload,f.payload),sha256(COALESCE(p_payload,f.payload)),
  COALESCE(p_envelope,f.envelope),sha256(COALESCE(p_envelope,f.envelope)),f.manifest_sha,f.signature,t.wire_id,t.public_digest,
  t.jwk_digest,m.wire_id,m.public_digest,p.identity_digest,f.hwm1,
  (SELECT seal_epoch FROM kb_vault_control WHERE singleton=1));
END $$;

-- Every writer independently rejects generation 0 and 2 without side effects.
SET LOCAL ROLE aimee_kb_jwks_publish;
DO $$ DECLARE g BIGINT; f p5c2b_fixture%ROWTYPE; BEGIN
 SELECT * INTO STRICT f FROM p5c2b_fixture;
 FOREACH g IN ARRAY ARRAY[0,2] LOOP
  BEGIN PERFORM pg_temp.p5c2b_stage(g); RAISE EXCEPTION 'stage generation % accepted',g;
   EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN PERFORM kb_management_jwks_publication_record_cas(g,f.candidate_id,f.hwm2);
   RAISE EXCEPTION 'CAS generation % accepted',g; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN PERFORM kb_management_jwks_publication_finalize(g,f.candidate_id);
   RAISE EXCEPTION 'finalize generation % accepted',g; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN PERFORM * FROM kb_management_jwks_manifest_key_admit(f.use_id,g,f.candidate_id,
   'p5c2b-manifest-custody','p5-jwks-root-v1-test',
   sha256(f.payload),decode(repeat('25',64),'hex'));
   RAISE EXCEPTION 'admit generation % accepted',g; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
 END LOOP;
 BEGIN PERFORM pg_temp.p5c2b_stage(1,decode(repeat('00',1024),'hex'),NULL,NULL);
  RAISE EXCEPTION 'oversized JWKS accepted'; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
 BEGIN PERFORM pg_temp.p5c2b_stage(1,NULL,decode(repeat('00',2048),'hex'),NULL);
  RAISE EXCEPTION 'oversized payload accepted'; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
 BEGIN PERFORM pg_temp.p5c2b_stage(1,NULL,NULL,decode(repeat('00',3072),'hex'));
  RAISE EXCEPTION 'oversized envelope accepted'; EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
RESET ROLE;
DO $$ BEGIN IF EXISTS(SELECT 1 FROM kb_management_jwks_publication_candidate) OR
 EXISTS(SELECT 1 FROM kb_management_jwks_manifest_key_use_intent) THEN
 RAISE EXCEPTION 'generation negatives mutated state'; END IF; END $$;

CREATE FUNCTION pg_temp.p5c2b_fail_use_audit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF NEW.action='vault.key_use' AND NEW.actor_principal='management-jwks-publisher' THEN
  RAISE EXCEPTION 'injected P5-C2b use audit failure'; END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER p5c2b_fail_use_audit BEFORE INSERT ON kb_audit_outbox
 FOR EACH ROW EXECUTE FUNCTION pg_temp.p5c2b_fail_use_audit();
DO $$ DECLARE f p5c2b_fixture%ROWTYPE; m TEXT; BEGIN
 SELECT * INTO STRICT f FROM p5c2b_fixture;
 SELECT wire_id INTO STRICT m FROM kb_management_token_root WHERE root_kind='manifest';
 BEGIN
  PERFORM * FROM kb_management_jwks_manifest_key_admit(f.use_id,1,f.candidate_id,
   'p5c2b-manifest-custody',m,sha256(f.payload),decode(repeat('25',64),'hex'));
  RAISE EXCEPTION 'failed use audit admitted key';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM<>'injected P5-C2b use audit failure' THEN RAISE; END IF;
 END;
 IF EXISTS(SELECT 1 FROM kb_management_jwks_manifest_key_use_intent) THEN
  RAISE EXCEPTION 'failed use audit left intent'; END IF;
END $$;
DROP TRIGGER p5c2b_fail_use_audit ON kb_audit_outbox;

CREATE FUNCTION pg_temp.p5c2b_fail_publish_audit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF NEW.action='management.jwks.publish' THEN
  RAISE EXCEPTION 'injected P5-C2b publish audit failure'; END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER p5c2b_fail_publish_audit BEFORE INSERT ON kb_audit_outbox
 FOR EACH ROW EXECUTE FUNCTION pg_temp.p5c2b_fail_publish_audit();

SET LOCAL ROLE aimee_kb_jwks_publish;
DO $$ DECLARE f p5c2b_fixture%ROWTYPE; m TEXT; n BOOLEAN; ep BIGINT;
 w1 BYTEA; w2 BYTEA; nn BYTEA; ct BYTEA; tg BYTEA; att BYTEA; roots RECORD;
 new_use TEXT; original_epoch BIGINT; alt_payload BYTEA; alt_use TEXT; BEGIN
 SELECT * INTO STRICT f FROM p5c2b_fixture;
 SELECT * INTO STRICT roots FROM kb_management_jwks_publication_roots();
 m:=roots.manifest_wire_id;
 SELECT newly_admitted,seal_epoch,wrapped_dek,nonce,ciphertext,tag,hwm_attestation
  INTO n,ep,w1,nn,ct,tg,att FROM kb_management_jwks_manifest_key_admit(
   f.use_id,1,f.candidate_id,'p5c2b-manifest-custody',m,sha256(f.payload),decode(repeat('25',64),'hex'));
 IF n IS DISTINCT FROM true OR ep<>roots.seal_epoch OR octet_length(w1)<>40 OR att<>decode(repeat('25',64),'hex') THEN
  RAISE EXCEPTION 'fresh manifest admission mismatch'; END IF;
 original_epoch:=ep;
 SELECT newly_admitted,wrapped_dek,hwm_attestation INTO n,w2,att
  FROM kb_management_jwks_manifest_key_admit(f.use_id,1,f.candidate_id,
   'p5c2b-manifest-custody',m,sha256(f.payload),decode(repeat('25',64),'hex'));
 IF n IS DISTINCT FROM false OR w2<>w1 OR att<>decode(repeat('25',64),'hex') THEN
  RAISE EXCEPTION 'manifest admission replay did not return exact envelope'; END IF;
 alt_payload:=convert_to('{"format_version":1,"generation":1,"changed":true}','UTF8');
 alt_use:=pg_temp.p5c2b_use_id(f.candidate_id,alt_payload,ep);
 BEGIN
  PERFORM * FROM kb_management_jwks_manifest_key_admit(alt_use,1,f.candidate_id,
   'p5c2b-manifest-custody',m,sha256(alt_payload),decode(repeat('25',64),'hex'));
  RAISE EXCEPTION 'same-epoch changed payload admitted';
 EXCEPTION WHEN unique_violation THEN NULL; END;
 BEGIN
  PERFORM * FROM kb_management_jwks_manifest_key_admit(f.use_id,1,f.candidate_id,
   'changed-custody-root',m,sha256(f.payload),decode(repeat('25',64),'hex'));
  RAISE EXCEPTION 'same-use changed root admitted';
 EXCEPTION WHEN unique_violation THEN NULL; END;
 BEGIN
  PERFORM * FROM kb_management_jwks_manifest_key_admit(f.use_id,1,repeat('d',64),
   'p5c2b-manifest-custody',m,sha256(f.payload),decode(repeat('25',64),'hex'));
  RAISE EXCEPTION 'same-use changed candidate admitted';
 EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
 PERFORM pg_temp.p5c2b_stage(1);
 IF EXISTS(SELECT 1 FROM kb_management_jwks_publication_final()) THEN
  RAISE EXCEPTION 'staged candidate leaked through final reader'; END IF;
 PERFORM pg_temp.p5c2b_bump_epoch();
 SELECT * INTO STRICT roots FROM kb_management_jwks_publication_roots();
 IF roots.seal_epoch<>original_epoch+1 THEN RAISE EXCEPTION 'seal epoch did not advance'; END IF;
 new_use:=pg_temp.p5c2b_use_id(f.candidate_id,f.payload,roots.seal_epoch);
 SELECT newly_admitted,seal_epoch,wrapped_dek,hwm_attestation INTO n,ep,w2,att
  FROM kb_management_jwks_manifest_key_admit(new_use,1,f.candidate_id,
   'p5c2b-manifest-custody',roots.manifest_wire_id,sha256(f.payload),decode(repeat('25',64),'hex'));
 IF n IS DISTINCT FROM true OR ep<>roots.seal_epoch OR w2<>w1 OR att<>decode(repeat('25',64),'hex') THEN
  RAISE EXCEPTION 'post-seal STAGED admission mismatch'; END IF;
 SELECT newly_admitted,wrapped_dek INTO n,w2 FROM kb_management_jwks_manifest_key_admit(
  new_use,1,f.candidate_id,'p5c2b-manifest-custody',roots.manifest_wire_id,
  sha256(f.payload),decode(repeat('25',64),'hex'));
 IF n IS DISTINCT FROM false OR w2<>w1 THEN RAISE EXCEPTION 'post-seal admission replay mismatch'; END IF;
 PERFORM kb_management_jwks_publication_record_cas(1,f.candidate_id,f.hwm2);
 PERFORM kb_management_jwks_publication_record_cas(1,f.candidate_id,f.hwm2);
 BEGIN PERFORM kb_management_jwks_publication_finalize(1,f.candidate_id);
  RAISE EXCEPTION 'failed publication audit finalized';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM<>'injected P5-C2b publish audit failure' THEN RAISE; END IF;
 END;
 IF EXISTS(SELECT 1 FROM kb_management_jwks_publication_final()) THEN
  RAISE EXCEPTION 'failed publication audit leaked final row'; END IF;
END $$;
RESET ROLE;
DROP TRIGGER p5c2b_fail_publish_audit ON kb_audit_outbox;
SET LOCAL ROLE aimee_kb_jwks_publish;
DO $$ DECLARE f p5c2b_fixture%ROWTYPE; BEGIN
 SELECT * INTO STRICT f FROM p5c2b_fixture;
 PERFORM kb_management_jwks_publication_finalize(1,f.candidate_id);
 PERFORM kb_management_jwks_publication_finalize(1,f.candidate_id);
 IF (SELECT count(*) FROM kb_management_jwks_publication_final())<>1 THEN
  RAISE EXCEPTION 'final reader cardinality mismatch'; END IF;
END $$;
RESET ROLE;

-- FINAL is immutable across later seal epochs; inspect preserves the original
-- stage epoch while roots reports the current open epoch.
UPDATE kb_vault_control SET seal_epoch=seal_epoch+1,updated_at=pg_now_text() WHERE singleton=1;
SET LOCAL ROLE aimee_kb_jwks_publish;
DO $$ DECLARE r RECORD; ep BIGINT; BEGIN
 SELECT seal_epoch INTO ep FROM kb_management_jwks_publication_roots();
 SELECT * INTO STRICT r FROM kb_management_jwks_publication_inspect();
 IF r.phase<>'final' OR r.seal_epoch>=ep OR
    (SELECT count(*) FROM kb_management_jwks_publication_final())<>1 THEN
  RAISE EXCEPTION 'post-epoch FINAL read mismatch'; END IF;
END $$;
RESET ROLE;

DO $$ BEGIN
 IF (SELECT count(*) FROM kb_management_jwks_publication_candidate WHERE phase='final')<>1 OR
    (SELECT count(*) FROM kb_management_jwks_publication_generation)<>1 OR
    (SELECT count(*) FROM kb_management_jwks_publication_registry)<>1 OR
    (SELECT count(*) FROM kb_management_jwks_manifest_key_use_intent)<>2 OR
    (SELECT count(*) FROM kb_audit_outbox WHERE action='vault.key_use' AND actor_principal='management-jwks-publisher')<>2 OR
    (SELECT count(*) FROM kb_audit_outbox WHERE action='management.jwks.publish')<>1 THEN
  RAISE EXCEPTION 'P5-C2b final/WORM cardinality mismatch'; END IF;
 BEGIN UPDATE kb_management_jwks_publication_candidate SET envelope_bytes='x' WHERE generation=1;
  RAISE EXCEPTION 'candidate mutation accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
 BEGIN DELETE FROM kb_management_jwks_publication_generation;
  RAISE EXCEPTION 'generation delete accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
 BEGIN TRUNCATE kb_management_jwks_publication_registry;
  RAISE EXCEPTION 'registry truncate accepted'; EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL; END;
 BEGIN UPDATE kb_management_jwks_manifest_key_use_intent SET purpose='x';
  RAISE EXCEPTION 'intent mutation accepted'; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;

ROLLBACK;
\echo 'P5-C2b JWKS publication PostgreSQL 17 gate passed'
