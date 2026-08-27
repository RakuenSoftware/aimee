-- P7 anchor-authoritative rotation persistence and isolation gate.
\set ON_ERROR_STOP on
BEGIN;

SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES (970701,'p7_rotation_a'),(970702,'p7_rotation_b');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES
  ('oidc:test:p7a',970701,1),('oidc:test:p7b',970702,1);

SELECT org_vault_put('team:970701:provider:bedrock',970701,'bedrock','primary',1,
  '\x0102'::bytea,'\x0304'::bytea,'\x0506'::bytea,'\x0708'::bytea);

DO $$
DECLARE rid BIGINT; audits BIGINT;
BEGIN
  rid := org_vault_rotation_start('owner','team:970701|bedrock|primary',
    'team:970701:provider:bedrock',970701,'bedrock','primary',1,false);
  IF org_vault_rotation_stage('owner',rid,'\x1112','\x1314','\x1516','\x1718') <> 2 THEN
    RAISE EXCEPTION 'P7 FAIL: stage did not return N+1';
  END IF;
  IF org_vault_has('team:970701:provider:bedrock','bedrock','primary') <> 1 THEN
    RAISE EXCEPTION 'P7 FAIL: stage moved current pointer before anchor CAS';
  END IF;
  IF (SELECT count(*) FROM org_vault_secret WHERE principal='team:970701:provider:bedrock') <> 2 THEN
    RAISE EXCEPTION 'P7 FAIL: immutable N+1 row missing';
  END IF;
  PERFORM org_vault_rotation_transition('owner',rid,'staged','probed','');
  BEGIN
    PERFORM org_vault_rotation_transition('owner',rid,'probed','failed','late failure');
    RAISE EXCEPTION 'P7 FAIL: probed rotation could race activation into failed';
  EXCEPTION WHEN invalid_parameter_value THEN NULL;
  END;
  PERFORM org_vault_rotation_transition('owner',rid,'probed','activating','');
  PERFORM org_vault_rotation_finalize('owner',rid,'\xaabbcc'::bytea);
  IF org_vault_has('team:970701:provider:bedrock','bedrock','primary') <> 2 THEN
    RAISE EXCEPTION 'P7 FAIL: finalize did not advance pointer';
  END IF;
  IF (SELECT hwm_attestation FROM org_vault_secret WHERE
      principal='team:970701:provider:bedrock' AND agent='bedrock' AND cred='primary' AND version=2)
      <> '\xaabbcc'::bytea THEN
    RAISE EXCEPTION 'P7 FAIL: final signed attestation missing';
  END IF;
  SELECT count(*) INTO audits FROM kb_audit_outbox WHERE action='vault.rotation.activate';
  PERFORM org_vault_rotation_finalize('owner',rid,'\xaabbcc'::bytea);
  IF (SELECT count(*) FROM kb_audit_outbox WHERE action='vault.rotation.activate') <> audits THEN
    RAISE EXCEPTION 'P7 FAIL: idempotent finalize duplicated WORM audit';
  END IF;
  BEGIN
    PERFORM org_vault_rotation_finalize('owner',rid,'\xdead'::bytea);
    RAISE EXCEPTION 'P7 FAIL: finalize accepted mismatched replay attestation';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  BEGIN
    PERFORM org_vault_put('team:970701:provider:bedrock',970701,'bedrock','primary',3,
      '\x01','\x02','\x03','\x04');
    RAISE EXCEPTION 'P7 FAIL: ordinary put bypassed active rotation';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  BEGIN
    PERFORM org_vault_rewrap('team:970701:provider:bedrock','bedrock','primary',2,'\x99');
    RAISE EXCEPTION 'P7 FAIL: rewrap bypassed active rotation';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  BEGIN
    PERFORM org_vault_delete('team:970701:provider:bedrock','bedrock','primary');
    RAISE EXCEPTION 'P7 FAIL: delete bypassed active rotation';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
END $$;

SELECT org_vault_put('team:970701:provider:bedrock',970701,'bedrock','secondary',1,
  '\x2102'::bytea,'\x2304'::bytea,'\x2506'::bytea,'\x2708'::bytea);
DO $$
BEGIN
  BEGIN
    PERFORM org_vault_rotation_start('owner','team:970701|bedrock|primary',
      'team:970701:provider:bedrock',970701,'bedrock','secondary',1,false);
    RAISE EXCEPTION 'P7 FAIL: HWM key was shared across credential slots';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
  BEGIN
    PERFORM org_vault_rotation_start('owner','different-hwm-key',
      'team:970701:provider:bedrock',970701,'bedrock','primary',2,false);
    RAISE EXCEPTION 'P7 FAIL: credential slot changed its stable HWM key';
  EXCEPTION WHEN serialization_failure THEN NULL;
  END;
END $$;

SELECT set_config('aimee.test_rotation_id',
  (SELECT id::text FROM org_vault_rotation WHERE team_id=970701 LIMIT 1),true);

DO $$
BEGIN
  BEGIN
    PERFORM org_vault_rotation_start('owner','mismatched-team',
      'team:970701:provider:bedrock',970702,'bedrock','primary',2,false);
    RAISE EXCEPTION 'P7 FAIL: admin bypassed principal/team binding';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;

SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:p7b',970702);
DO $$
DECLARE rid BIGINT;
BEGIN
  BEGIN
    PERFORM id FROM public.org_vault_rotation;
    RAISE EXCEPTION 'P7 FAIL: runtime retained direct rotation SELECT';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  SELECT id INTO rid FROM org_vault_rotation_get(
    current_setting('aimee.test_rotation_id')::bigint);
  IF rid IS NOT NULL THEN RAISE EXCEPTION 'P7 FAIL: definer rotation row leaked cross-tenant'; END IF;
  BEGIN
    PERFORM org_vault_rotation_start('oidc:test:p7b','foreign-key',
      'team:970701:provider:bedrock',970701,'bedrock','other',1,false);
    RAISE EXCEPTION 'P7 FAIL: cross-tenant rotation start succeeded';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;

SELECT set_tenant_context('oidc:test:p7a',970701);
DO $$
DECLARE rid BIGINT; got BIGINT;
BEGIN
  rid := current_setting('aimee.test_rotation_id')::bigint;
  SELECT id INTO got FROM org_vault_rotation_get(rid);
  IF rid IS NULL OR got IS DISTINCT FROM rid THEN
    RAISE EXCEPTION 'P7 FAIL: own-team rotation unavailable';
  END IF;
END $$;

RESET ROLE;
\echo '== P7 rotation persistence + isolation assertions PASSED =='
ROLLBACK;
