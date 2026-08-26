\set ON_ERROR_STOP on
BEGIN;

DO $$
DECLARE owner_oid OID;
BEGIN
  SELECT oid INTO owner_oid FROM pg_roles WHERE rolname='aimee_kb_owner';
  IF owner_oid IS NULL THEN RAISE EXCEPTION 'bootstrap owner role missing'; END IF;
  IF (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
       WHERE n.nspname='public' AND p.proname IN
        ('kb_management_status_key_bootstrap_stage',
         'kb_management_status_key_bootstrap_resume',
         'kb_management_status_key_bootstrap_prepare_activation',
         'kb_management_status_key_bootstrap_finalize')
       AND p.prosecdef AND p.proowner=owner_oid
       AND p.proconfig @> ARRAY['search_path=pg_catalog, pg_temp']::text[]) <> 4 THEN
    RAISE EXCEPTION 'bootstrap functions are not fixed owner hardened definers';
  END IF;
  IF NOT has_function_privilege('aimee_kb_migrate',
      'kb_management_status_key_bootstrap_resume(text,text)','EXECUTE')
     OR NOT has_function_privilege('aimee_kb_migrate',
      'kb_management_status_key_bootstrap_stage(text,text,text,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea,bytea)','EXECUTE')
     OR has_function_privilege('aimee_kb_runtime',
      'kb_management_status_key_bootstrap_resume(text,text)','EXECUTE')
     OR has_function_privilege('aimee_kb_status',
      'kb_management_status_key_bootstrap_resume(text,text)','EXECUTE')
     OR has_function_privilege('aimee_kb_status_login',
      'kb_management_status_key_bootstrap_resume(text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'bootstrap execute ACL mismatch';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname='aimee_kb_status_login' AND
      (rolcanlogin OR rolinherit OR rolbypassrls OR rolsuper OR rolcreatedb OR
       rolcreaterole OR rolreplication)) THEN
    RAISE EXCEPTION 'status login attributes are unsafe';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_auth_members m JOIN pg_roles parent ON parent.oid=m.roleid
      JOIN pg_roles child ON child.oid=m.member
      WHERE parent.rolname='aimee_kb_status' AND child.rolname='aimee_kb_status_login') THEN
    RAISE EXCEPTION 'status login is not pinned to status role';
  END IF;
  IF (SELECT pg_get_constraintdef(oid) FROM pg_constraint
       WHERE conrelid='org_vault_rotation'::regclass
         AND conname='org_vault_rotation_from_version_check') NOT LIKE '%from_version >= 1%' THEN
    RAISE EXCEPTION 'generic rotation from-version constraint weakened';
  END IF;
END $$;

SET LOCAL ROLE aimee_kb_migrate;
SELECT set_config('p5b1b.bootstrap_id',encode(sha256(convert_to(
  'aimee-p5-status-bootstrap-v1|platform:p5-status','UTF8')),'hex'),true);
DO $$ DECLARE x RECORD; BEGIN
  SELECT * INTO x FROM kb_management_status_key_bootstrap_resume(
    current_setting('p5b1b.bootstrap_id'),'platform:p5-status');
  IF x.state<>'empty' OR x.seal_epoch<1 THEN
    RAISE EXCEPTION 'empty inspect did not return open seal epoch';
  END IF;
END $$;
SELECT * FROM kb_management_status_key_bootstrap_stage(
  current_setting('p5b1b.bootstrap_id'),'platform:p5-status','status-v1',decode(repeat('21',32),'hex'),
  decode(repeat('a1',64),'hex'),
  decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),
  decode(repeat('13',32),'hex'),decode(repeat('14',16),'hex'),
  decode(repeat('21',40),'hex'),decode(repeat('22',12),'hex'),
  decode(repeat('23',32),'hex'),decode(repeat('24',16),'hex'),
  sha256(decode(repeat('21',32),'hex')),decode(repeat('d1',32),'hex'),
  decode(repeat('d2',32),'hex'));

DO $$
DECLARE x RECORD; n BIGINT;
BEGIN
  SELECT * INTO x FROM kb_management_status_key_bootstrap_resume(
    current_setting('p5b1b.bootstrap_id'),'platform:p5-status');
  IF x.state<>'staged' OR x.from_version<>1 OR x.to_version<>2 OR x.enabled
     OR x.seal_epoch<1
     OR octet_length(x.public_key)<>32 OR octet_length(x.hwm1_attestation)<>64
     OR x.hwm2_attestation IS NOT NULL OR octet_length(x.wrapped_dek)<>40
     OR octet_length(x.ciphertext)<>32 OR octet_length(x.public_key_digest)<>32
     OR octet_length(x.v1_envelope_digest)<>32 OR octet_length(x.v2_envelope_digest)<>32
     OR x.public_key_digest<>sha256(decode(repeat('21',32),'hex'))
     OR x.v1_envelope_digest<>decode(repeat('d1',32),'hex')
     OR x.v2_envelope_digest<>decode(repeat('d2',32),'hex')
     OR octet_length(x.v1_wrapped_dek)<>40 OR octet_length(x.v1_ciphertext)<>32 THEN
    RAISE EXCEPTION 'staged bootstrap shape mismatch';
  END IF;
  IF (SELECT version FROM org_vault_current WHERE principal='org:p5-status'
      AND agent='management' AND cred='ed25519')<>1 THEN
    RAISE EXCEPTION 'staging did not leave inert current v1';
  END IF;
  SELECT count(*) INTO n FROM kb_audit_outbox WHERE action IN
    ('vault.rotation.start','vault.rotation.stage');
  PERFORM * FROM kb_management_status_key_bootstrap_stage(
    current_setting('p5b1b.bootstrap_id'),'platform:p5-status','status-v1',decode(repeat('21',32),'hex'),
    decode(repeat('a1',64),'hex'),decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),
    decode(repeat('13',32),'hex'),decode(repeat('14',16),'hex'),decode(repeat('21',40),'hex'),
    decode(repeat('22',12),'hex'),decode(repeat('23',32),'hex'),decode(repeat('24',16),'hex'),
    sha256(decode(repeat('21',32),'hex')),decode(repeat('d1',32),'hex'),
    decode(repeat('d2',32),'hex'));
  IF (SELECT count(*) FROM kb_audit_outbox WHERE action IN
      ('vault.rotation.start','vault.rotation.stage'))<>n THEN
    RAISE EXCEPTION 'exact stage replay duplicated audit';
  END IF;
  BEGIN
    PERFORM * FROM kb_management_status_key_bootstrap_stage(
      current_setting('p5b1b.bootstrap_id'),'platform:p5-status','status-v1',decode(repeat('31',32),'hex'),
      decode(repeat('a1',64),'hex'),decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),
      decode(repeat('13',32),'hex'),decode(repeat('14',16),'hex'),decode(repeat('21',40),'hex'),
      decode(repeat('22',12),'hex'),decode(repeat('23',32),'hex'),decode(repeat('24',16),'hex'),
      sha256(decode(repeat('31',32),'hex')),decode(repeat('d1',32),'hex'),
      decode(repeat('d2',32),'hex'));
    RAISE EXCEPTION 'changed stage replay accepted';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
  BEGIN
    PERFORM * FROM kb_management_status_key_bootstrap_stage(
      current_setting('p5b1b.bootstrap_id'),'platform:p5-status','status-v1',decode(repeat('21',32),'hex'),
      decode(repeat('a1',64),'hex'),decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),
      decode(repeat('13',32),'hex'),decode(repeat('14',16),'hex'),decode(repeat('21',40),'hex'),
      decode(repeat('22',12),'hex'),decode(repeat('23',32),'hex'),decode(repeat('24',16),'hex'),
      sha256(decode(repeat('21',32),'hex')),decode(repeat('e1',32),'hex'),
      decode(repeat('d2',32),'hex'));
    RAISE EXCEPTION 'changed canonical digest replay accepted';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
END $$;

DO $$
DECLARE x RECORD;
BEGIN
  SELECT * INTO x FROM kb_management_status_key_bootstrap_prepare_activation(current_setting('p5b1b.bootstrap_id'));
  IF x.state<>'activating' OR x.expected_version<>1 OR x.next_version<>2 THEN
    RAISE EXCEPTION 'prepare activation mismatch';
  END IF;
  SELECT * INTO x FROM kb_management_status_key_bootstrap_prepare_activation(current_setting('p5b1b.bootstrap_id'));
  IF x.state<>'activating' THEN RAISE EXCEPTION 'prepare replay mismatch'; END IF;
END $$;

-- A failed WORM append must not publish v2 or enable the registry.
RESET ROLE;
CREATE FUNCTION pg_temp.p5b1b_fail_audit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'injected bootstrap audit failure'; END $$;
CREATE TRIGGER p5b1b_fail_audit BEFORE INSERT ON kb_audit_outbox
  FOR EACH ROW EXECUTE FUNCTION pg_temp.p5b1b_fail_audit();
SET LOCAL ROLE aimee_kb_migrate;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM kb_management_status_key_bootstrap_finalize(
      current_setting('p5b1b.bootstrap_id'),decode(repeat('a2',64),'hex'));
    RAISE EXCEPTION 'audit failure published bootstrap';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM<>'injected bootstrap audit failure' THEN RAISE; END IF;
  END;
END $$;
RESET ROLE;
DROP TRIGGER p5b1b_fail_audit ON kb_audit_outbox;
DO $$ BEGIN
  IF (SELECT state FROM org_vault_rotation WHERE principal='org:p5-status')<>'activating'
     OR (SELECT version FROM org_vault_current WHERE principal='org:p5-status'
       AND agent='management' AND cred='ed25519')<>1
     OR (SELECT enabled FROM kb_management_status_key WHERE singleton=1)
     OR (SELECT hwm_attestation FROM org_vault_secret WHERE principal='org:p5-status'
       AND agent='management' AND cred='ed25519' AND version=2) IS NOT NULL THEN
    RAISE EXCEPTION 'audit rollback was not atomic';
  END IF;
END $$;

SET LOCAL ROLE aimee_kb_migrate;
DO $$
DECLARE x RECORD; n BIGINT;
BEGIN
  SELECT * INTO x FROM kb_management_status_key_bootstrap_finalize(
    current_setting('p5b1b.bootstrap_id'),decode(repeat('a2',64),'hex'));
  IF x.state<>'activated' OR x.version<>2 OR octet_length(x.public_key)<>32 THEN
    RAISE EXCEPTION 'finalize result mismatch';
  END IF;
  SELECT count(*) INTO n FROM kb_audit_outbox WHERE action='vault.rotation.activate';
  PERFORM * FROM kb_management_status_key_bootstrap_finalize(
    current_setting('p5b1b.bootstrap_id'),decode(repeat('a2',64),'hex'));
  IF (SELECT count(*) FROM kb_audit_outbox WHERE action='vault.rotation.activate')<>n THEN
    RAISE EXCEPTION 'finalize replay duplicated audit';
  END IF;
END $$;
RESET ROLE;

DO $$ BEGIN
  IF (SELECT version FROM org_vault_current WHERE principal='org:p5-status'
      AND agent='management' AND cred='ed25519')<>2
     OR NOT (SELECT enabled FROM kb_management_status_key WHERE singleton=1)
     OR (SELECT state FROM org_vault_rotation WHERE principal='org:p5-status')<>'activated'
     OR octet_length((SELECT hwm_attestation FROM org_vault_secret
       WHERE principal='org:p5-status' AND agent='management'
         AND cred='ed25519' AND version=2))<>64 THEN
    RAISE EXCEPTION 'bootstrap did not publish exact v2 state';
  END IF;
END $$;

SET LOCAL ROLE aimee_kb_runtime;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM kb_management_status_key_bootstrap_resume(
      current_setting('p5b1b.bootstrap_id'),'platform:p5-status');
    RAISE EXCEPTION 'runtime invoked owner bootstrap';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;
RESET ROLE;

ROLLBACK;
\echo 'p5b1b status bootstrap PG17 tests passed'
