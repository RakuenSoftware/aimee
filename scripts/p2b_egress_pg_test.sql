-- P2b-a durable egress authority PostgreSQL correctness and privilege gate.
\set ON_ERROR_STOP on
BEGIN;
SELECT set_config('aimee.principal','owner',true);

INSERT INTO kb_team(id,name) VALUES (982001,'p2b_alpha');
INSERT INTO kb_project(id,parent,name) VALUES (982010,982001,'p2b_project');
INSERT INTO kb_team_membership(identity_key,team,is_default)
  VALUES ('cert:issuer-a:01',982001,1);
INSERT INTO kb_project_membership(identity_key,project)
  VALUES ('cert:issuer-a:01',982010);
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,cert_issuer,cert_serial_norm,authority_id)
  VALUES ('p2b','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','01',
          'active','issuer-a','01','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb');

SELECT org_catalog_bedrock_upsert('p2b-model','P2b Model','converse','anthropic',
  'foundation','aws','us-east-1',NULL,ARRAY['us-east-1'],NULL,'',true);
SELECT org_model_entitle('p2b-model',982001);
SELECT org_pricing_add_version('bedrock','p2b-billable',1.0,2.0,0.5,0.75);
SELECT org_vault_put('team:982001:bedrock',982001,'bedrock','iam',1,
  '\x01'::bytea,'\x0102030405060708090a0b0c'::bytea,'\x02'::bytea,
  '\x0102030405060708090a0b0c0d0e0f10'::bytea);
INSERT INTO org_vault_rotation(key_id,principal,team_id,agent,cred,from_version,to_version,state)
  VALUES ('p2b-key','team:982001:bedrock',982001,'bedrock','iam',1,2,'activated');
SELECT org_budget_set(982001,NULL,'day',100.0,NULL);
SELECT org_egress_binding_set(982001,'p2b-model','p2b-billable',1,'p2b-key',
  'team:982001:bedrock','bedrock','iam',1000,100,true);

SELECT set_config('aimee.principal','cert:issuer-a:01',true);
DO $$
DECLARE a RECORD; b RECORD; ok BOOLEAN; n BIGINT; st TEXT; spend NUMERIC; reserved NUMERIC;
BEGIN
  SELECT * INTO a FROM org_egress_admit(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'issuer-a','01','cert:issuer-a:01','11111111-1111-4111-8111-111111111111',
    982001,982010,'p2b-model',
    'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',60);
  IF a.outcome <> 'admitted' OR a.dispatch_state <> 'admitted' OR a.dispatch_id IS NULL THEN
    RAISE EXCEPTION 'P2b FAIL: initial admission = %/%/%',a.outcome,a.dispatch_state,a.dispatch_id;
  END IF;
  SELECT * INTO b FROM org_egress_admit(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'issuer-a','01','cert:issuer-a:01','11111111-1111-4111-8111-111111111111',
    982001,982010,'p2b-model',
    'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',60);
  IF b.outcome <> 'replay' OR b.dispatch_id <> a.dispatch_id THEN
    RAISE EXCEPTION 'P2b FAIL: exact replay did not return durable row';
  END IF;
  BEGIN
    PERFORM * FROM org_egress_admit(
      'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'issuer-a','01','cert:issuer-a:01','11111111-1111-4111-8111-111111111111',
      982001,NULL,'p2b-model',
      'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',60);
    RAISE EXCEPTION 'P2b FAIL: mismatched replay accepted';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
  SELECT * INTO b FROM org_egress_dispatch_begin(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','11111111-1111-4111-8111-111111111111',
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee','gate-instance',60);
  IF b.dispatch_id <> a.dispatch_id OR b.owner_generation <> 1 THEN
    RAISE EXCEPTION 'P2b FAIL: ownership claim wrong';
  END IF;
  ok := org_egress_dispatch_heartbeat(a.dispatch_id,
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',1,60);
  IF NOT ok THEN RAISE EXCEPTION 'P2b FAIL: live heartbeat refused'; END IF;
  IF org_egress_dispatch_heartbeat(a.dispatch_id,
    'ffffffffffffffffffffffffffffffff',1,60) THEN
    RAISE EXCEPTION 'P2b FAIL: wrong owner heartbeat accepted';
  END IF;
  ok := org_egress_dispatch_settle(a.dispatch_id,
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',1,'succeeded',200,10,5,0,0,'complete');
  IF NOT ok THEN RAISE EXCEPTION 'P2b FAIL: owner settle refused'; END IF;
  IF org_egress_dispatch_settle(a.dispatch_id,
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',1,'succeeded',200,10,5,0,0,'again') THEN
    RAISE EXCEPTION 'P2b FAIL: terminal double settle accepted';
  END IF;
  SELECT state INTO st FROM org_egress_dispatch WHERE id=a.dispatch_id;
  IF st <> 'succeeded' THEN RAISE EXCEPTION 'P2b FAIL: terminal state=%',st; END IF;
  SELECT spend_usd,reserved_usd INTO spend,reserved FROM org_budget_counter
    WHERE team_id=982001 AND project_id IS NULL AND period='day';
  IF spend <= 0 OR reserved <> 0 THEN
    RAISE EXCEPTION 'P2b FAIL: budget settlement spend=% reserved=%',spend,reserved;
  END IF;
  BEGIN
    UPDATE org_egress_dispatch SET outcome_class='forged' WHERE id=a.dispatch_id;
    RAISE EXCEPTION 'P2b FAIL: terminal WORM mutation accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;

  -- Stale admitted recovers to failed and releases its reservation at zero.
  SELECT * INTO a FROM org_egress_admit(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'issuer-a','01','cert:issuer-a:01','22222222-2222-4222-8222-222222222222',
    982001,982010,'p2b-model',
    'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',1);
  UPDATE org_egress_dispatch SET lease_expires_at=now()-interval '1 minute' WHERE id=a.dispatch_id;
  n := org_egress_recover(100);
  IF n < 1 OR (SELECT state FROM org_egress_dispatch WHERE id=a.dispatch_id) <> 'failed' THEN
    RAISE EXCEPTION 'P2b FAIL: stale admitted recovery';
  END IF;
END $$;

-- The primary enrollment row is authoritative on every new admission.
UPDATE kb_enrollments SET state='revoked',revoked_at=pg_now_text()
  WHERE authority_id='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb';
DO $$ BEGIN
  BEGIN
    PERFORM * FROM org_egress_admit(
      'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'issuer-a','01','cert:issuer-a:01','33333333-3333-4333-8333-333333333333',
      982001,982010,'p2b-model',
      'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',60);
    RAISE EXCEPTION 'P2b FAIL: revoked enrollment admitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;

SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('cert:issuer-a:01',982001);
DO $$ BEGIN
  BEGIN
    PERFORM * FROM org_egress_dispatch;
    RAISE EXCEPTION 'P2b FAIL: runtime directly read dispatch ledger';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  BEGIN
    UPDATE org_egress_binding SET enabled=false;
    RAISE EXCEPTION 'P2b FAIL: runtime directly mutated binding';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;
RESET ROLE;
\echo '== P2b egress authority PostgreSQL assertions PASSED =='
ROLLBACK;
