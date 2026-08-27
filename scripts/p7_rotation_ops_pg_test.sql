-- P7 fenced vendor-operation workflow, recovery, and WORM gate.
\set ON_ERROR_STOP on
BEGIN;

SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES (970711,'p7_ops_a'),(970712,'p7_ops_b');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES
  ('oidc:test:p7opsa',970711,1),('oidc:test:p7opsb',970712,1);

SELECT org_vault_put('team:970711:provider:bedrock',970711,'bedrock','primary',1,
  decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),
  '\x7365637265742d6d61726b6572'::bytea,decode(repeat('03',16),'hex'));

DO $$
DECLARE rid BIGINT; tok1 BIGINT; tok2 BIGINT; got BIGINT; uses BIGINT;
BEGIN
  rid := org_vault_rotation_start('owner','team:970711|bedrock|primary',
    'team:970711:provider:bedrock',970711,'bedrock','primary',1,false);
  tok1 := org_vault_rotation_claim('owner',rid,'provision','worker-a',30);
  BEGIN
    PERFORM org_vault_rotation_claim('owner',rid,'provision','worker-a',30);
    RAISE EXCEPTION 'P7 OPS FAIL: claim acquisition doubled as unfenced renewal';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  IF NOT org_vault_rotation_heartbeat('owner',rid,'worker-a',tok1,30) THEN
    RAISE EXCEPTION 'P7 OPS FAIL: explicit fenced heartbeat failed';
  END IF;
  PERFORM org_vault_rotation_checkpoint_old_ref('owner',rid,'worker-a',tok1,'old-ref');
  IF NOT org_vault_rotation_release('owner',rid,'worker-a',tok1) THEN
    RAISE EXCEPTION 'P7 OPS FAIL: release failed';
  END IF;
  tok2 := org_vault_rotation_claim('owner',rid,'provision','worker-b',30);
  IF tok2 <= tok1 THEN RAISE EXCEPTION 'P7 OPS FAIL: fencing token did not advance'; END IF;
  BEGIN
    PERFORM org_vault_rotation_checkpoint_old_ref('owner',rid,'worker-a',tok1,'old-ref');
    RAISE EXCEPTION 'P7 OPS FAIL: stale token mutated rotation';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  BEGIN
    PERFORM org_vault_rotation_checkpoint_old_ref('owner',rid,NULL,NULL,'old-ref');
    RAISE EXCEPTION 'P7 OPS FAIL: NULL claim bypassed fencing';
  EXCEPTION WHEN invalid_parameter_value THEN NULL;
  END;
  PERFORM org_vault_rotation_checkpoint_old_ref('owner',rid,'worker-b',tok2,'old-ref');
  got := org_vault_rotation_stage_claimed('owner',rid,'worker-b',tok2,'new-ref',
    decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),
    '\x7365637265742d6d61726b6572'::bytea,decode(repeat('13',16),'hex'));
  IF got<>2 THEN RAISE EXCEPTION 'P7 OPS FAIL: claimed stage did not return N+1'; END IF;
  tok2 := org_vault_rotation_claim('owner',rid,'staged','worker-b',30);
  SELECT count(*) INTO got FROM org_vault_rotation_probe_admit(
    'owner',rid,'worker-b',tok2,'p7:probe:one');
  IF got<>1 THEN RAISE EXCEPTION 'P7 OPS FAIL: probe admission returned no envelope'; END IF;
  SELECT count(*) INTO uses FROM kb_audit_outbox
    WHERE action='vault.rotation.probe_use' AND subject='p7:probe:one';
  PERFORM * FROM org_vault_rotation_probe_admit(
    'owner',rid,'worker-b',tok2,'p7:probe:one');
  IF (SELECT count(*) FROM kb_audit_outbox WHERE action='vault.rotation.probe_use' AND
      subject='p7:probe:one')<>uses THEN
    RAISE EXCEPTION 'P7 OPS FAIL: probe admission duplicated WORM use';
  END IF;
  PERFORM org_vault_rotation_transition_claimed(
    'owner',rid,'worker-b',tok2,'staged','probed','');
  PERFORM org_vault_rotation_transition('owner',rid,'probed','activating','');
  PERFORM org_vault_rotation_finalize('owner',rid,'\xaabbcc'::bytea);
  tok2 := org_vault_rotation_claim('owner',rid,'activated','worker-b',30);
  BEGIN
    PERFORM org_vault_rotation_transition_claimed(
      'owner',rid,'worker-b',tok2,'activated','revoked','');
    RAISE EXCEPTION 'P7 OPS FAIL: revoke without evidence succeeded';
  EXCEPTION WHEN invalid_parameter_value THEN NULL;
  END;
  PERFORM org_vault_rotation_transition_claimed(
    'owner',rid,'worker-b',tok2,'activated','revoked','confirmed-unusable');
  tok2 := org_vault_rotation_claim('owner',rid,'revoked','worker-b',30);
  PERFORM org_vault_rotation_transition_claimed(
    'owner',rid,'worker-b',tok2,'revoked','retired','');
  IF (SELECT state FROM org_vault_rotation WHERE id=rid)<>'retired' THEN
    RAISE EXCEPTION 'P7 OPS FAIL: success workflow did not retire';
  END IF;
END $$;

-- A definite post-provision failure must remove only inert N+1 after vendor
-- reconciliation and an anchor-authoritative N read.
SELECT org_vault_put('team:970711:provider:cohere',970711,'cohere','primary',1,
  decode(repeat('21',40),'hex'),decode(repeat('22',12),'hex'),'\x23'::bytea,
  decode(repeat('24',16),'hex'));
DO $$
DECLARE rid BIGINT; tok BIGINT;
BEGIN
  rid := org_vault_rotation_start('owner','team:970711|cohere|primary',
    'team:970711:provider:cohere',970711,'cohere','primary',1,false);
  tok := org_vault_rotation_claim('owner',rid,'provision','worker-r',30);
  PERFORM org_vault_rotation_checkpoint_old_ref('owner',rid,'worker-r',tok,'old-cohere');
  PERFORM org_vault_rotation_stage_claimed('owner',rid,'worker-r',tok,'new-cohere',
    decode(repeat('31',40),'hex'),decode(repeat('32',12),'hex'),'\x33'::bytea,
    decode(repeat('34',16),'hex'));
  tok := org_vault_rotation_claim('owner',rid,'staged','worker-r',30);
  PERFORM org_vault_rotation_fail_claimed(
    'owner',rid,'worker-r',tok,'staged','probe','definite failure');
  tok := org_vault_rotation_claim('owner',rid,'failed','worker-r',30);
  BEGIN
    PERFORM org_vault_rotation_remediate(
      'owner',rid,'worker-r',tok,2,'confirmed-unusable');
    RAISE EXCEPTION 'P7 OPS FAIL: remediation accepted advanced anchor';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  PERFORM org_vault_rotation_remediate(
    'owner',rid,'worker-r',tok,1,'confirmed-unusable');
  IF org_vault_has('team:970711:provider:cohere','cohere','primary')<>1 OR
     EXISTS(SELECT 1 FROM org_vault_secret WHERE principal='team:970711:provider:cohere'
       AND version=2) THEN
    RAISE EXCEPTION 'P7 OPS FAIL: remediation changed N or retained inert N+1';
  END IF;
END $$;

SELECT set_config('aimee.test_rotation_ops_id',
  (SELECT id::text FROM org_vault_rotation WHERE team_id=970711 ORDER BY id LIMIT 1),true);
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:p7opsb',970712);
DO $$
BEGIN
  BEGIN
    PERFORM org_vault_rotation_claim('oidc:test:p7opsb',
      current_setting('aimee.test_rotation_ops_id')::bigint,'retired','foreign',30);
    RAISE EXCEPTION 'P7 OPS FAIL: cross-tenant claim succeeded';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;
RESET ROLE;

DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM kb_audit_outbox WHERE action LIKE 'vault.rotation.%' AND
      (detail ILIKE '%secret-marker%' OR detail ILIKE '%7365637265742d6d61726b6572%')) THEN
    RAISE EXCEPTION 'P7 OPS FAIL: secret material appeared in WORM metadata';
  END IF;
END $$;

\echo '== P7 fenced rotation operations assertions PASSED =='
ROLLBACK;
