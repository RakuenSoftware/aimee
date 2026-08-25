\set ON_ERROR_STOP on
BEGIN;

DO $$
DECLARE status_oid OID; definer_oid OID;
BEGIN
  SELECT oid INTO status_oid FROM pg_roles WHERE rolname='aimee_kb_status';
  SELECT oid INTO definer_oid FROM pg_roles WHERE rolname='aimee_kb_status_definer';
  IF EXISTS (SELECT 1 FROM pg_roles WHERE oid=status_oid AND
      (rolcanlogin OR rolinherit OR rolsuper OR rolcreatedb OR rolcreaterole OR rolreplication
       OR rolbypassrls)) THEN
    RAISE EXCEPTION 'status role has dangerous attributes';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_auth_members WHERE member=status_oid) THEN
    RAISE EXCEPTION 'status role inherits an upstream role';
  END IF;
  -- A future pinned listener login may be made a downstream member of this
  -- execution role; no other downstream role is permitted.
  IF EXISTS (SELECT 1 FROM pg_auth_members m JOIN pg_roles r ON r.oid=m.member
      WHERE m.roleid=status_oid AND r.rolname<>'aimee_kb_status_login') THEN
    RAISE EXCEPTION 'status role has an unpinned downstream member';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_owner' AND rolbypassrls) THEN
    RAISE EXCEPTION 'global owner unexpectedly bypasses RLS';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE oid=definer_oid AND
      (rolcanlogin OR rolinherit OR NOT rolbypassrls OR rolsuper OR rolcreatedb OR rolcreaterole
       OR rolreplication)) THEN
    RAISE EXCEPTION 'status definer role attributes are not hardened';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_auth_members WHERE member=definer_oid OR roleid=definer_oid) THEN
    RAISE EXCEPTION 'status definer role has memberships';
  END IF;
  IF NOT has_function_privilege('aimee_kb_status',
      'kb_management_status_key_candidate(text,text,bigint)','EXECUTE')
     OR NOT has_function_privilege('aimee_kb_status',
      'kb_management_status_key_admit(text,text,text,bigint,text,text,text,text,text,text,bigint,bytea)',
      'EXECUTE')
     OR NOT has_function_privilege('aimee_kb_status',
      'kb_management_status_key_use_guard(bigint)','EXECUTE') THEN
    RAISE EXCEPTION 'status role lacks fixed key authority functions';
  END IF;
  IF has_table_privilege('aimee_kb_status','org_vault_secret','SELECT')
     OR has_table_privilege('aimee_kb_status','kb_management_status_key_use_intent','SELECT')
     OR has_function_privilege('aimee_kb_status',
      'org_vault_key_use_candidate(text,bigint,text,text,text,text,bigint)','EXECUTE') THEN
    RAISE EXCEPTION 'status role has generic/raw vault authority';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname LIKE 'kb_management_status_key_%'
        AND p.proname NOT LIKE 'kb_management_status_key_bootstrap_%'
        AND p.prosecdef AND NOT (p.proconfig @>
          ARRAY['search_path=pg_catalog, public, pg_temp']::text[])) THEN
    RAISE EXCEPTION 'status definer search_path is not hardened';
  END IF;
  IF (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
       WHERE n.nspname='public' AND p.proname IN
        ('kb_management_status_key_candidate','kb_management_status_key_admit',
         'kb_management_status_key_use_guard','kb_management_status_key_startup_status')
       AND p.prosecdef AND p.proowner=definer_oid) <> 4 THEN
    RAISE EXCEPTION 'status definer owner is not fixed';
  END IF;
  IF (SELECT count(*) FROM pg_proc WHERE proowner=definer_oid) <> 4
     OR EXISTS (SELECT 1 FROM pg_class WHERE relowner=definer_oid) THEN
    RAISE EXCEPTION 'status definer owns objects outside the four fixed functions';
  END IF;
  IF (SELECT count(DISTINCT (table_name,privilege_type))
        FROM information_schema.role_table_grants
       WHERE grantee='aimee_kb_status_definer' AND table_schema='public') <> 17
     OR EXISTS (
      SELECT table_name,privilege_type
        FROM information_schema.role_table_grants
       WHERE grantee='aimee_kb_status_definer' AND table_schema='public'
      EXCEPT VALUES
       ('kb_management_status_key','SELECT'),('kb_management_status_key','UPDATE'),
       ('kb_management_status_key_use_intent','SELECT'),
       ('kb_management_status_key_use_intent','INSERT'),
       ('kb_management_status_key_use_intent','UPDATE'),
       ('org_vault_current','SELECT'),('org_vault_secret','SELECT'),
       ('org_vault_secret','UPDATE'),('org_vault_rotation','SELECT'),
       ('kb_enrollments','SELECT'),('kb_enrollments','UPDATE'),
       ('kb_server_registry','SELECT'),('kb_server_registry','UPDATE'),
       ('kb_team_membership','SELECT'),('kb_team_membership','UPDATE'),
       ('kb_cert_revocation_generation','SELECT'),
       ('kb_cert_revocation_generation','UPDATE')) THEN
    RAISE EXCEPTION 'status definer has excess table authority';
  END IF;
  IF NOT has_function_privilege('aimee_kb_status_definer',
      'org_vault_control_require_open()','EXECUTE')
     OR NOT has_function_privilege('aimee_kb_status_definer',
      'org_vault_control_startup_status()','EXECUTE')
     OR NOT has_function_privilege('aimee_kb_status_definer',
      'kb_audit_worm_append(text,text,text,text,text,text)','EXECUTE')
     OR has_function_privilege('aimee_kb_status_definer',
      'org_vault_key_use_candidate(text,bigint,text,text,text,text,bigint)','EXECUTE') THEN
    RAISE EXCEPTION 'status definer helper authority is not exact';
  END IF;
END $$;

DO $$
DECLARE s RECORD;
BEGIN
  SELECT * INTO s FROM kb_management_status_key_startup_status();
  IF s.seal_epoch<1 OR s.sealed OR s.enabled OR s.custody_key_id IS NOT NULL
     OR s.wire_key_id IS NOT NULL OR s.public_key IS NOT NULL OR s.version IS NOT NULL
     OR s.hwm_attestation IS NOT NULL THEN
    RAISE EXCEPTION 'empty startup status shape mismatch';
  END IF;
END $$;

INSERT INTO kb_management_status_key_use_intent(
  use_id,custody_key_id,wire_key_id,version,request_digest,hwm_attestation_digest,
  caller_issuer,caller_serial_norm,
  caller_fingerprint,target_server_id,target_mgmt_fingerprint,revocation_generation,seal_epoch)
SELECT repeat('1',64),'platform:p5-status','status-1',1,repeat('2',64),
       encode(sha256('x'::bytea),'hex'),'test-issuer','01',
       repeat('3',64),'test-server',repeat('4',64),1,seal_epoch
  FROM kb_vault_control WHERE singleton=1;

DO $$ BEGIN
  BEGIN UPDATE kb_management_status_key_use_intent SET version=2 WHERE use_id=repeat('1',64);
    RAISE EXCEPTION 'WORM update allowed'; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN DELETE FROM kb_management_status_key_use_intent WHERE use_id=repeat('1',64);
    RAISE EXCEPTION 'WORM delete allowed'; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN TRUNCATE kb_management_status_key_use_intent;
    RAISE EXCEPTION 'WORM truncate allowed'; EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;

-- Exercise the production admission function against a complete fresh fixture.
INSERT INTO kb_team(id,name) VALUES(975102,'p5b1-pg17-fixture');
INSERT INTO kb_team_membership(identity_key,team,is_default)
 VALUES('cert:fixture-issuer:0a',975102,1);
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,expires_at,revoked_at,legacy,
 cert_issuer,cert_serial_norm,authority_id) VALUES
 ('p5-kb-management',repeat('a',64),'0a','active','2099-01-01T00:00:00Z','',0,
  'fixture-issuer','0a',repeat('a',32)),
 ('p5-server-management',repeat('b',64),'0b','active','2099-01-01T00:00:00Z','',0,
  'fixture-target-issuer','0b',repeat('b',32));
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
 mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
 VALUES('fixture-target','fixture-client','fixture-mgmt',975102,'https://127.0.0.1:2','active',
 'fixture-target-issuer','0b',repeat('b',64));
INSERT INTO org_vault_secret(principal,team_id,agent,cred,version,wrapped_dek,nonce,ciphertext,
 tag,hwm_attestation) VALUES('org:p5-status',NULL,'management','ed25519',2,
 decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),decode(repeat('13',32),'hex'),
 decode(repeat('14',16),'hex'),'\xaabbcc'::bytea);
INSERT INTO org_vault_current(principal,agent,cred,version)
 VALUES('org:p5-status','management','ed25519',2);
INSERT INTO org_vault_rotation(key_id,principal,team_id,agent,cred,from_version,to_version,state)
 VALUES('platform:p5-status','org:p5-status',NULL,'management','ed25519',1,2,'activated');
INSERT INTO kb_management_status_key(singleton,bootstrap_id,custody_key_id,wire_key_id,public_key,enabled)
 VALUES(1,repeat('9',64),'platform:p5-status','status-1',decode(repeat('44',32),'hex'),true);
SELECT set_config('p5b1.generation',generation::text,true)
  FROM kb_cert_revocation_generation WHERE singleton=1;

SET LOCAL ROLE aimee_kb_status;
DO $$
DECLARE r RECORD;
BEGIN
  SELECT * INTO r FROM kb_management_status_key_admit(
    repeat('5',64),'platform:p5-status','status-1',2,repeat('6',64),
    'fixture-issuer','0a',repeat('a',64),'fixture-target',repeat('b',64),
    current_setting('p5b1.generation')::bigint,'\xaabbcc'::bytea);
  IF NOT r.newly_admitted OR r.seal_epoch<1 OR octet_length(r.wrapped_dek)<>40
     OR octet_length(r.nonce)<>12 OR octet_length(r.ciphertext)<>32
     OR octet_length(r.tag)<>16 OR r.hwm_attestation<>'\xaabbcc'::bytea THEN
    RAISE EXCEPTION 'fresh admission did not return exact envelope';
  END IF;
END $$;
RESET ROLE;

DO $$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM kb_management_status_key_use_intent
      WHERE use_id=repeat('5',64)
        AND hwm_attestation_digest=encode(sha256('\xaabbcc'::bytea),'hex')) THEN
    RAISE EXCEPTION 'fresh admission did not persist attestation-bound intent';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM kb_audit_outbox
      WHERE action='management.status.sign.v1' AND subject='fixture-target'
        AND detail LIKE '%"hwm_attestation_digest"%'
        AND detail LIKE '%'||encode(sha256('\xaabbcc'::bytea),'hex')||'%') THEN
    RAISE EXCEPTION 'fresh admission did not persist attestation-bound WORM audit';
  END IF;
END $$;

SET LOCAL ROLE aimee_kb_status;
DO $$
DECLARE r RECORD;
BEGIN
  SELECT * INTO r FROM kb_management_status_key_admit(
    repeat('5',64),'platform:p5-status','status-1',2,repeat('6',64),
    'fixture-issuer','0a',repeat('a',64),'fixture-target',repeat('b',64),
    current_setting('p5b1.generation')::bigint,'\xaabbcc'::bytea);
  IF r.newly_admitted OR r.wrapped_dek IS NOT NULL OR r.hwm_attestation IS NOT NULL THEN
    RAISE EXCEPTION 'fresh exact replay exposed envelope';
  END IF;
  BEGIN
    PERFORM * FROM kb_management_status_key_admit(
      repeat('5',64),'platform:p5-status','status-1',2,repeat('6',64),
      'fixture-issuer','0a',repeat('a',64),'fixture-target',repeat('b',64),
      current_setting('p5b1.generation')::bigint,'\xaabbcd'::bytea);
    RAISE EXCEPTION 'changed-attestation replay was admitted';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
END $$;
RESET ROLE;

CREATE FUNCTION pg_temp.p5b1_fail_audit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'injected audit failure'; END $$;
CREATE TRIGGER p5b1_fail_audit BEFORE INSERT ON kb_audit_outbox
  FOR EACH ROW EXECUTE FUNCTION pg_temp.p5b1_fail_audit();
SET LOCAL ROLE aimee_kb_status;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM kb_management_status_key_admit(
      repeat('7',64),'platform:p5-status','status-1',2,repeat('8',64),
      'fixture-issuer','0a',repeat('a',64),'fixture-target',repeat('b',64),
      current_setting('p5b1.generation')::bigint,'\xaabbcc'::bytea);
    RAISE EXCEPTION 'audit failure returned an envelope';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM <> 'injected audit failure' THEN RAISE; END IF;
  END;
END $$;
RESET ROLE;
DROP TRIGGER p5b1_fail_audit ON kb_audit_outbox;
DO $$ BEGIN
  IF EXISTS (SELECT 1 FROM kb_management_status_key_use_intent WHERE use_id=repeat('7',64)) THEN
    RAISE EXCEPTION 'audit failure did not roll back intent';
  END IF;
END $$;

CREATE TEMP TABLE kb_management_status_key(singleton smallint,custody_key_id text,
  wire_key_id text,enabled boolean);

SET LOCAL ROLE aimee_kb_status;
DO $$
DECLARE r RECORD;
BEGIN
  IF EXISTS (SELECT 1 FROM kb_management_status_key_candidate(
      'platform:p5-status','status-1',1)) THEN
    RAISE EXCEPTION 'temporary schema shadow reached definer';
  END IF;
  SELECT * INTO r FROM kb_management_status_key_admit(
    repeat('1',64),'platform:p5-status','status-1',1,repeat('2',64),'test-issuer','01',
    repeat('3',64),'test-server',repeat('4',64),1,'x'::bytea);
  IF r.newly_admitted OR r.seal_epoch<1 OR r.wrapped_dek IS NOT NULL
     OR r.nonce IS NOT NULL OR r.ciphertext IS NOT NULL OR r.tag IS NOT NULL
     OR r.hwm_attestation IS NOT NULL THEN
    RAISE EXCEPTION 'exact replay exposed an envelope';
  END IF;
  PERFORM kb_management_status_key_use_guard(r.seal_epoch);
END $$;

ROLLBACK;
\echo 'p5b1 status key PG17 tests passed'
