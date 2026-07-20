-- P7 signed-HWM steady-state key-use admission and replay gate.
\set ON_ERROR_STOP on
BEGIN;

SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES (970711,'p7_key_use_a'),(970712,'p7_key_use_b');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES
  ('oidc:test:p7usea',970711,1),('oidc:test:p7useb',970712,1);

SELECT org_vault_put('team:970711:provider:bedrock',970711,'bedrock','primary',1,
  decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),decode('03','hex'),
  decode(repeat('04',16),'hex'));

DO $$
DECLARE rid BIGINT;
BEGIN
  rid := org_vault_rotation_start('owner','team:970711|bedrock|primary',
    'team:970711:provider:bedrock',970711,'bedrock','primary',1,false);
  PERFORM org_vault_rotation_stage('owner',rid,decode(repeat('11',40),'hex'),
    decode(repeat('12',12),'hex'),decode(repeat('13',23),'hex'),decode(repeat('14',16),'hex'));
  PERFORM org_vault_rotation_transition('owner',rid,'staged','probed','');
  PERFORM org_vault_rotation_transition('owner',rid,'probed','activating','');
  PERFORM org_vault_rotation_finalize('owner',rid,'\xaabbcc'::bytea);
END $$;

SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:p7usea',970711);

DO $$
DECLARE n BOOLEAN; w BYTEA; a BYTEA; audits BIGINT;
BEGIN
  BEGIN
    PERFORM 1 FROM org_vault_key_use_intent;
    RAISE EXCEPTION 'P7 key-use FAIL: runtime retained direct intent read';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  IF (SELECT count(*) FROM org_vault_key_use_candidate('oidc:test:p7usea',970711,
      'team:970711|bedrock|primary','team:970711:provider:bedrock','bedrock','primary',2)) <> 1 THEN
    RAISE EXCEPTION 'P7 key-use FAIL: signed current candidate unavailable';
  END IF;
  SELECT newly_admitted,wrapped_dek,hwm_attestation INTO n,w,a FROM org_vault_key_use_admit(
    'oidc:test:p7usea',970711,'cert:test-ca:01','use-1','team:970711|bedrock|primary',
    'team:970711:provider:bedrock','bedrock','primary',2,repeat('a',64),
    'bedrock','anthropic.claude','invoke','\xaabbcc'::bytea);
  IF n IS DISTINCT FROM true OR octet_length(w)<>40 OR a<>'\xaabbcc'::bytea THEN
    RAISE EXCEPTION 'P7 key-use FAIL: new admission did not return exact envelope';
  END IF;
  SELECT newly_admitted,wrapped_dek,hwm_attestation INTO n,w,a FROM org_vault_key_use_admit(
    'oidc:test:p7usea',970711,'cert:test-ca:01','use-1','team:970711|bedrock|primary',
    'team:970711:provider:bedrock','bedrock','primary',2,repeat('a',64),
    'bedrock','anthropic.claude','invoke','\xaabbcc'::bytea);
  IF n IS DISTINCT FROM false OR w IS NOT NULL OR a IS NOT NULL THEN
    RAISE EXCEPTION 'P7 key-use FAIL: exact replay returned an envelope';
  END IF;
  BEGIN
    PERFORM * FROM org_vault_key_use_admit('oidc:test:p7usea',970711,
      'cert:test-ca:01','use-1','team:970711|bedrock|primary',
      'team:970711:provider:bedrock','bedrock','primary',2,repeat('b',64),
      'bedrock','anthropic.claude','invoke','\xaabbcc'::bytea);
    RAISE EXCEPTION 'P7 key-use FAIL: conflicting replay accepted';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
  BEGIN
    PERFORM * FROM org_vault_key_use_admit('oidc:test:p7usea',970711,
      'cert:test-ca:01','use-bad-att','team:970711|bedrock|primary',
      'team:970711:provider:bedrock','bedrock','primary',2,repeat('c',64),
      'bedrock','anthropic.claude','invoke','\xdead'::bytea);
    RAISE EXCEPTION 'P7 key-use FAIL: mismatched stored attestation accepted';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  BEGIN
    PERFORM * FROM org_vault_key_use_candidate('oidc:test:p7usea',970711,
      'team:970711|bedrock|primary','team:970711:provider:bedrock','bedrock','primary',1);
    RAISE EXCEPTION 'P7 key-use FAIL: stale anchor version accepted';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
END $$;

RESET ROLE;
DO $$
BEGIN
  IF (SELECT count(*) FROM org_vault_key_use_intent WHERE team_id=970711)<>1 THEN
    RAISE EXCEPTION 'P7 key-use FAIL: intent cardinality mismatch';
  END IF;
  IF (SELECT count(*) FROM kb_audit_event WHERE action='vault.key_use' AND
      subject='team:970711|bedrock|primary')<>1 THEN
    RAISE EXCEPTION 'P7 key-use FAIL: WORM audit cardinality mismatch';
  END IF;
END $$;

SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:p7useb',970712);
DO $$
BEGIN
  BEGIN
    PERFORM * FROM org_vault_key_use_candidate('oidc:test:p7useb',970711,
      'team:970711|bedrock|primary','team:970711:provider:bedrock','bedrock','primary',2);
    RAISE EXCEPTION 'P7 key-use FAIL: cross-team candidate accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;

RESET ROLE;
\echo '== P7 signed-HWM key-use admission assertions PASSED =='
ROLLBACK;
