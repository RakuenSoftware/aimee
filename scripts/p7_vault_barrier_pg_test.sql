-- P7 primary vault barrier, privilege, inventory, and epoch gate.
\set ON_ERROR_STOP on
BEGIN;

CREATE OR REPLACE FUNCTION p7_expect_sealed(p_sql TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$
BEGIN
  BEGIN
    EXECUTE p_sql;
    RAISE EXCEPTION 'P7 barrier FAIL: guarded statement succeeded: %',p_sql;
  EXCEPTION WHEN SQLSTATE '55000' THEN
    IF SQLERRM<>'org_vault_control: sealed' THEN
      RAISE EXCEPTION 'P7 barrier FAIL: unstable sealed classification: %',SQLERRM;
    END IF;
  END;
END; $$;

DO $$
DECLARE c kb_vault_control%ROWTYPE;
BEGIN
  SELECT * INTO STRICT c FROM kb_vault_control;
  IF c.singleton<>1 OR c.sealed OR c.seal_epoch<1 OR c.maintenance_kind<>'' OR
     c.maintenance_id<>'' OR c.fencing_token<0 THEN
    RAISE EXCEPTION 'P7 barrier FAIL: bad singleton defaults';
  END IF;
  BEGIN
    INSERT INTO kb_vault_control(singleton) VALUES(2);
    RAISE EXCEPTION 'P7 barrier FAIL: second singleton accepted';
  EXCEPTION WHEN check_violation THEN NULL;
  END;
  BEGIN
    UPDATE kb_vault_control SET seal_epoch=0 WHERE singleton=1;
    RAISE EXCEPTION 'P7 barrier FAIL: nonpositive epoch accepted';
  EXCEPTION WHEN check_violation THEN NULL;
  END;
  BEGIN
    UPDATE kb_vault_control SET fencing_token=-1 WHERE singleton=1;
    RAISE EXCEPTION 'P7 barrier FAIL: negative fence accepted';
  EXCEPTION WHEN check_violation THEN NULL;
  END;
  BEGIN
    UPDATE kb_vault_control SET sealed=false,maintenance_kind='reseal',maintenance_id='op-1'
      WHERE singleton=1;
    RAISE EXCEPTION 'P7 barrier FAIL: unsealed maintenance identity accepted';
  EXCEPTION WHEN check_violation THEN NULL;
  END;
END $$;

DELETE FROM kb_vault_control WHERE singleton=1;
SELECT p7_expect_sealed('SELECT org_vault_control_require_open()');
DO $$ BEGIN
  IF (SELECT count(*) FROM org_vault_control_startup_status())<>0 THEN
    RAISE EXCEPTION 'P7 barrier FAIL: startup status fabricated missing singleton';
  END IF;
END $$;
INSERT INTO kb_vault_control(singleton,sealed,seal_epoch) VALUES(1,true,2);
SELECT p7_expect_sealed('SELECT org_vault_control_require_open()');
DO $$ DECLARE ep BIGINT; is_sealed BOOLEAN;
BEGIN
  SELECT seal_epoch,sealed INTO STRICT ep,is_sealed FROM org_vault_control_startup_status();
  IF ep<>2 OR is_sealed IS DISTINCT FROM true THEN
    RAISE EXCEPTION 'P7 barrier FAIL: sealed startup status mismatch';
  END IF;
END $$;
UPDATE kb_vault_control SET maintenance_kind='reseal',maintenance_id='op-1' WHERE singleton=1;
SELECT p7_expect_sealed('SELECT org_vault_control_require_open()');
UPDATE kb_vault_control SET sealed=false,seal_epoch=7,maintenance_kind='',maintenance_id=''
 WHERE singleton=1;
DO $$ DECLARE ep BIGINT; is_sealed BOOLEAN;
BEGIN
  SELECT seal_epoch,sealed INTO STRICT ep,is_sealed FROM org_vault_control_startup_status();
  IF ep<>7 OR is_sealed IS DISTINCT FROM false THEN
    RAISE EXCEPTION 'P7 barrier FAIL: open startup status mismatch';
  END IF;
END $$;

SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES(970721,'p7_barrier');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES('owner',970721,0);
SELECT org_vault_salt_ensure('team:970721:provider:bedrock','\x0102'::bytea);
SELECT org_vault_kek_check_set('team:970721:provider:bedrock','\x0304'::bytea);
SELECT org_vault_put('team:970721:provider:bedrock',970721,'bedrock','primary',1,
  decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),decode('03','hex'),
  decode(repeat('04',16),'hex'));

SELECT set_config('aimee.p7_barrier_rid',org_vault_rotation_start(
  'owner','team:970721|bedrock|primary','team:970721:provider:bedrock',970721,
  'bedrock','primary',1,false)::text,true);
SELECT org_vault_rotation_stage('owner',current_setting('aimee.p7_barrier_rid')::bigint,
  decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),decode('13','hex'),
  decode(repeat('14',16),'hex'));
SELECT org_vault_rotation_transition('owner',current_setting('aimee.p7_barrier_rid')::bigint,
  'staged','probed','');
SELECT org_vault_rotation_transition('owner',current_setting('aimee.p7_barrier_rid')::bigint,
  'probed','activating','');
SELECT org_vault_rotation_finalize('owner',current_setting('aimee.p7_barrier_rid')::bigint,
  '\xaabbcc'::bytea);

DO $$ DECLARE n BOOLEAN; ep BIGINT; w BYTEA;
BEGIN
  SELECT newly_admitted,seal_epoch,wrapped_dek INTO n,ep,w FROM org_vault_key_use_admit(
    'owner',970721,'cert:test-ca:barrier','use-replay','team:970721|bedrock|primary',
    'team:970721:provider:bedrock','bedrock','primary',2,repeat('a',64),
    'bedrock','anthropic.claude','invoke','\xaabbcc'::bytea);
  IF n IS DISTINCT FROM true OR ep<>7 OR octet_length(w)<>40 THEN
    RAISE EXCEPTION 'P7 barrier FAIL: new admission epoch/envelope mismatch';
  END IF;
  SELECT newly_admitted,seal_epoch,wrapped_dek INTO n,ep,w FROM org_vault_key_use_admit(
    'owner',970721,'cert:test-ca:barrier','use-replay','team:970721|bedrock|primary',
    'team:970721:provider:bedrock','bedrock','primary',2,repeat('a',64),
    'bedrock','anthropic.claude','invoke','\xaabbcc'::bytea);
  IF n IS DISTINCT FROM false OR ep<>7 OR w IS NOT NULL THEN
    RAISE EXCEPTION 'P7 barrier FAIL: open replay lost epoch or returned envelope';
  END IF;
  IF (SELECT seal_epoch FROM org_vault_key_use_intent WHERE team_id=970721 AND
      authenticated_origin='cert:test-ca:barrier' AND use_id='use-replay')<>7 OR
     (SELECT (detail::json->>'seal_epoch')::bigint FROM kb_audit_outbox
       WHERE action='vault.key_use' AND subject='team:970721|bedrock|primary'
       ORDER BY outbox_id DESC LIMIT 1)<>7 THEN
    RAISE EXCEPTION 'P7 barrier FAIL: durable intent/WORM epoch mismatch';
  END IF;
END $$;

-- The guarded set is deliberately exact and every member carries a direct call.
DO $$
DECLARE expected TEXT[] := ARRAY[
  'kb_management_jwks_manifest_key_admit',
  'kb_management_jwks_publication_final','kb_management_jwks_publication_finalize',
  'kb_management_jwks_publication_inspect','kb_management_jwks_publication_record_cas',
  'kb_management_jwks_publication_root_bind',
  'kb_management_jwks_publication_roots',
  'kb_management_jwks_publication_stage',
  'kb_management_jwks_runtime_fetch',
  'kb_management_status_key_admit','kb_management_status_key_bootstrap_finalize',
  'kb_management_status_key_bootstrap_prepare_activation',
  'kb_management_status_key_bootstrap_resume','kb_management_status_key_bootstrap_stage',
  'kb_management_status_key_candidate',
  'kb_management_status_key_use_guard',
  'kb_management_token_root_bootstrap_finalize',
  'kb_management_token_root_bootstrap_record_cas',
  'kb_management_token_root_bootstrap_resume',
  'kb_management_token_root_bootstrap_stage',
  'org_vault_delete','org_vault_kek_check_set','org_vault_key_use_admit','org_vault_put',
  'org_vault_rewrap','org_vault_rotation_checkpoint_old_ref','org_vault_rotation_claim',
  'org_vault_rotation_fail_claimed','org_vault_rotation_finalize',
  'org_vault_rotation_heartbeat','org_vault_rotation_probe_admit',
  'org_vault_rotation_release','org_vault_rotation_remediate','org_vault_rotation_stage',
  'org_vault_rotation_stage_claimed','org_vault_rotation_start',
  'org_vault_rotation_transition','org_vault_rotation_transition_claimed',
  'org_vault_salt_ensure'];
DECLARE actual TEXT[];
DECLARE writers TEXT[];
DECLARE expected_writers TEXT[] := ARRAY[
  'kb_management_status_key_bootstrap_finalize',
  'kb_management_status_key_bootstrap_prepare_activation',
  'kb_management_status_key_bootstrap_stage',
  'kb_management_token_root_bootstrap_finalize',
  'kb_management_token_root_bootstrap_record_cas',
  'kb_management_token_root_bootstrap_stage',
  'org_vault_delete','org_vault_kek_check_set','org_vault_key_use_admit','org_vault_put',
  'org_vault_rewrap','org_vault_rewrap_promote','org_vault_rotation_checkpoint_old_ref','org_vault_rotation_claim',
  'org_vault_rotation_fail_claimed','org_vault_rotation_finalize',
  'org_vault_rotation_heartbeat','org_vault_rotation_release','org_vault_rotation_remediate',
  'org_vault_rotation_stage','org_vault_rotation_stage_claimed','org_vault_rotation_start',
  'org_vault_rotation_transition','org_vault_rotation_transition_claimed',
  'org_vault_salt_ensure'];
DECLARE readonly TEXT[] := ARRAY['org_vault_current_wraps','org_vault_get_current',
  'org_vault_has','org_vault_kek_check_read','org_vault_key_use_candidate','org_vault_list',
  'org_vault_list_principals','org_vault_rotation_authorized','org_vault_rotation_get',
  'org_vault_salt_read'];
DECLARE fn RECORD;
DECLARE guard_pos INTEGER;
DECLARE sensitive_pos INTEGER;
BEGIN
  SELECT array_agg(DISTINCT p.proname ORDER BY p.proname) INTO actual FROM pg_proc p
   JOIN pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname='public' AND
   p.prosecdef AND p.prosrc LIKE '%org_vault_control_require_open()%';
  IF actual IS DISTINCT FROM expected THEN
    RAISE EXCEPTION 'P7 barrier FAIL: guarded inventory mismatch: %',actual;
  END IF;
  IF EXISTS(SELECT p.proname FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname=ANY(expected)
      GROUP BY p.proname HAVING count(*)<>1) THEN
    RAISE EXCEPTION 'P7 barrier FAIL: protected entrypoint overload drift';
  END IF;
  FOR fn IN SELECT p.proname,p.prosrc FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname=ANY(expected)
  LOOP
    guard_pos := position('org_vault_control_require_open()' IN lower(fn.prosrc));
    sensitive_pos := regexp_instr(lower(fn.prosrc),
      '(pg_advisory_xact_lock|for[[:space:]]+update|kb_audit_worm_append|insert[[:space:]]+into[[:space:]]+(public\.)?org_vault_|update[[:space:]]+(public\.)?org_vault_|delete[[:space:]]+from[[:space:]]+(public\.)?org_vault_)');
    IF guard_pos=0 OR (sensitive_pos>0 AND sensitive_pos<guard_pos) THEN
      RAISE EXCEPTION 'P7 barrier FAIL: guard ordering drift in % (guard %, sensitive %)',
        fn.proname,guard_pos,sensitive_pos;
    END IF;
  END LOOP;
  SELECT array_agg(DISTINCT p.proname ORDER BY p.proname) INTO writers FROM pg_proc p
   JOIN pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname='public' AND p.prosecdef AND
   p.prosrc ~* '(insert[[:space:]]+into|update|delete[[:space:]]+from)[[:space:]]+(public[.])?org_vault_(salt|secret|current|rotation|key_use_intent)';
  IF writers IS DISTINCT FROM expected_writers THEN
    RAISE EXCEPTION 'P7 barrier FAIL: authoritative writer inventory mismatch: %',writers;
  END IF;
  IF EXISTS(SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
    WHERE n.nspname='public' AND p.proname=ANY(readonly) AND
      p.prosrc LIKE '%org_vault_control_require_open()%') THEN
    RAISE EXCEPTION 'P7 barrier FAIL: read-only inventory became guarded';
  END IF;
  IF (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname=ANY(readonly) AND p.prosecdef) <>
      cardinality(readonly) OR
     EXISTS(SELECT p.proname FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname=ANY(readonly)
      GROUP BY p.proname HAVING count(*)<>1) THEN
    RAISE EXCEPTION 'P7 barrier FAIL: read-only function inventory drift';
  END IF;
  IF (SELECT array_agg(p.proname::TEXT ORDER BY p.proname::TEXT) FROM pg_proc p JOIN pg_namespace n
      ON n.oid=p.pronamespace WHERE n.nspname='public' AND
      p.proname LIKE 'org_vault_control_%') IS DISTINCT FROM
      ARRAY['org_vault_control_lock_exclusive','org_vault_control_require_open',
            'org_vault_control_startup_status']::TEXT[] THEN
    RAISE EXCEPTION 'P7 barrier FAIL: maintenance mutation API exposed';
  END IF;
  IF NOT EXISTS(SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname='org_vault_control_require_open' AND
        p.prosrc LIKE '%pg_advisory_xact_lock_shared(-7046029254386353131::BIGINT)%' AND
        position('pg_advisory_xact_lock_shared' IN p.prosrc) < position('FOR SHARE' IN p.prosrc)) OR
     NOT EXISTS(SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname='org_vault_control_lock_exclusive' AND
        p.prosrc LIKE '%pg_advisory_xact_lock(-7046029254386353131::BIGINT)%' AND
        position('pg_advisory_xact_lock(' IN p.prosrc) < position('FOR UPDATE' IN p.prosrc)) THEN
    RAISE EXCEPTION 'P7 barrier FAIL: control advisory domain/order drift';
  END IF;
  IF (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname IN
        ('org_vault_control_lock_exclusive','org_vault_control_require_open') AND
        p.prosecdef AND p.provolatile='v' AND
        p.proconfig @> ARRAY['search_path=public']::TEXT[])<>2 THEN
    RAISE EXCEPTION 'P7 barrier FAIL: control helper security attributes drift';
  END IF;
  IF NOT EXISTS(SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname='org_vault_control_startup_status' AND
        p.prosecdef AND p.provolatile='v' AND
        p.proconfig @> ARRAY['search_path=pg_catalog, public, pg_temp']::TEXT[] AND
        p.prosrc LIKE '%pg_advisory_xact_lock_shared(-7046029254386353131::BIGINT)%' AND
        p.prosrc LIKE '%public.kb_vault_control%' AND p.prosrc LIKE '%FOR SHARE%') OR
     EXISTS(SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
       CROSS JOIN LATERAL aclexplode(COALESCE(p.proacl,acldefault('f',p.proowner))) a
       WHERE n.nspname='public' AND p.proname='org_vault_control_startup_status' AND
         a.grantee=0 AND a.privilege_type='EXECUTE') OR
     NOT has_function_privilege('aimee_kb_runtime',
       'public.org_vault_control_startup_status()','EXECUTE') THEN
    RAISE EXCEPTION 'P7 barrier FAIL: startup status security/privilege drift';
  END IF;
  IF (SELECT column_default IS NOT NULL FROM information_schema.columns
      WHERE table_schema='public' AND table_name='org_vault_key_use_intent' AND
        column_name='seal_epoch') THEN
    RAISE EXCEPTION 'P7 barrier FAIL: seal_epoch insert must fail closed when omitted';
  END IF;
  IF EXISTS(SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname IN
        ('org_vault_rotation_transition_claimed','org_vault_rotation_fail_claimed') AND
        (length(p.prosrc)-length(replace(p.prosrc,'claim_until<=clock_timestamp()',''))) /
          length('claim_until<=clock_timestamp()') < 2) THEN
    RAISE EXCEPTION 'P7 barrier FAIL: claimed transition expiry recheck drift';
  END IF;
END $$;

INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
 client_issuer,client_serial_norm,client_fingerprint)
VALUES('p7-jwks-reader','p7-jwks-reader-client','p7-jwks-reader-mgmt',970721,
 'https://p7-jwks-reader.invalid','active','p7-jwks-reader-issuer','01',repeat('c',64));
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,expires_at,revoked_at,legacy,
 cert_issuer,cert_serial_norm,authority_id)
VALUES('service:aimee-server',repeat('c',64),'01','active','2999-01-01 00:00:00+00','',0,
 'p7-jwks-reader-issuer','01',repeat('c',32));

UPDATE kb_vault_control SET sealed=true,maintenance_kind='reseal',maintenance_id='op-guard'
 WHERE singleton=1;

-- The established vault entrypoints reject before their advisory/row locks or mutation;
-- P5-B1's fixed status entrypoints are exercised by p5b1-status-key-pg17-test.sql.
SELECT p7_expect_sealed($q$SELECT org_vault_salt_ensure('sealed-principal','\x01')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_kek_check_set('team:970721:provider:bedrock','\x01')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_put('sealed-principal',970721,'a','c',1,'\x01','\x02','\x03','\x04')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_delete('team:970721:provider:bedrock','bedrock','primary')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rewrap('team:970721:provider:bedrock','bedrock','primary',2,'\x01')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_start('owner','key-x','team:970721:provider:bedrock',970721,'bedrock','primary',2,false)$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_stage('owner',current_setting('aimee.p7_barrier_rid')::bigint,'\x01','\x02','\x03','\x04')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_transition('owner',current_setting('aimee.p7_barrier_rid')::bigint,'activated','revoked','')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_finalize('owner',current_setting('aimee.p7_barrier_rid')::bigint,'\x01')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_claim('owner',current_setting('aimee.p7_barrier_rid')::bigint,'activated','worker',30)$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_heartbeat('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1,30)$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_release('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1)$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_checkpoint_old_ref('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1,'ref')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_stage_claimed('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1,'ref',decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),'\x03',decode(repeat('04',16),'hex'))$q$);
SELECT p7_expect_sealed($q$SELECT * FROM org_vault_rotation_probe_admit('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1,'operation')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_transition_claimed('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1,'activated','revoked','receipt')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_fail_claimed('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1,'staged','probe','error')$q$);
SELECT p7_expect_sealed($q$SELECT org_vault_rotation_remediate('owner',current_setting('aimee.p7_barrier_rid')::bigint,'worker',1,1,'evidence')$q$);
SELECT p7_expect_sealed($q$SELECT * FROM org_vault_key_use_admit('owner',970721,'cert:test-ca:barrier','use-replay','team:970721|bedrock|primary','team:970721:provider:bedrock','bedrock','primary',2,repeat('a',64),'bedrock','anthropic.claude','invoke','\xaabbcc')$q$);
SELECT p7_expect_sealed($q$SELECT * FROM kb_management_jwks_publication_inspect()$q$);
SELECT p7_expect_sealed($q$SELECT * FROM kb_management_jwks_publication_roots()$q$);
SELECT p7_expect_sealed($q$SELECT * FROM kb_management_jwks_publication_final()$q$);
SELECT p7_expect_sealed($q$SELECT * FROM kb_management_jwks_runtime_fetch(
 'p7-jwks-reader-issuer','01',repeat('c',64))$q$);
SELECT p7_expect_sealed($q$SELECT kb_management_jwks_manifest_key_admit(repeat('a',64),1,repeat('b',64),'custody','manifest',decode(repeat('01',32),'hex'),'\x01')$q$);
SELECT p7_expect_sealed($q$SELECT kb_management_jwks_publication_record_cas(1,repeat('b',64),'\x01')$q$);
SELECT p7_expect_sealed($q$SELECT kb_management_jwks_publication_finalize(1,repeat('b',64))$q$);
SELECT p7_expect_sealed($q$SELECT kb_management_jwks_publication_stage(1,repeat('b',64),1,2,
 decode(repeat('00',32),'hex'),'\x01',sha256('\x01'::bytea),'\x02',sha256('\x02'::bytea),
 '\x03',sha256('\x03'::bytea),decode(repeat('04',32),'hex'),decode(repeat('05',64),'hex'),
 'token',decode(repeat('06',32),'hex'),decode(repeat('07',32),'hex'),'manifest',
 decode(repeat('08',32),'hex'),decode(repeat('09',32),'hex'),'\x0a',7)$q$);

DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM org_vault_salt WHERE principal='sealed-principal') OR
     (SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=970721)<>1 THEN
    RAISE EXCEPTION 'P7 barrier FAIL: sealed entrypoint left side effects';
  END IF;
  -- Ciphertext/metadata-only reads stay available while sealed.
  PERFORM org_vault_salt_read('team:970721:provider:bedrock');
  PERFORM org_vault_kek_check_read('team:970721:provider:bedrock');
  PERFORM * FROM org_vault_get_current('team:970721:provider:bedrock','bedrock','primary');
  PERFORM org_vault_has('team:970721:provider:bedrock','bedrock','primary');
  PERFORM * FROM org_vault_list('team:970721:provider:bedrock');
  PERFORM * FROM org_vault_list_principals();
  PERFORM * FROM org_vault_current_wraps('team:970721:provider:bedrock');
  PERFORM org_vault_rotation_authorized('owner',970721);
  PERFORM * FROM org_vault_rotation_get(current_setting('aimee.p7_barrier_rid')::bigint);
  PERFORM * FROM org_vault_key_use_candidate('owner',970721,'team:970721|bedrock|primary',
    'team:970721:provider:bedrock','bedrock','primary',2);
END $$;

SET ROLE aimee_kb_runtime;
DO $$ DECLARE ep BIGINT; is_sealed BOOLEAN;
BEGIN
  BEGIN PERFORM 1 FROM kb_vault_control;
    RAISE EXCEPTION 'P7 barrier FAIL: runtime read control row';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN PERFORM org_vault_control_require_open();
    RAISE EXCEPTION 'P7 barrier FAIL: runtime executed internal helper';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN PERFORM org_vault_control_lock_exclusive();
    RAISE EXCEPTION 'P7 barrier FAIL: runtime executed exclusive lock helper';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  SELECT seal_epoch,sealed INTO STRICT ep,is_sealed FROM org_vault_control_startup_status();
  IF ep<>7 OR is_sealed IS DISTINCT FROM true THEN
    RAISE EXCEPTION 'P7 barrier FAIL: runtime sealed startup status mismatch';
  END IF;
END $$;
RESET ROLE;

\echo '== P7 primary vault barrier assertions PASSED =='
ROLLBACK;
