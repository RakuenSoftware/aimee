-- P2b-a durable egress authority PostgreSQL correctness and privilege gate.
\set ON_ERROR_STOP on
BEGIN;
SELECT set_config('aimee.principal','owner',true);

INSERT INTO kb_team(id,name) VALUES (982001,'p2b_alpha');
INSERT INTO kb_project(id,parent,name) VALUES (982010,982001,'p2b_project');
INSERT INTO kb_project(id,parent,name) VALUES (982011,982001,'p2b_other_project');
INSERT INTO kb_team_membership(identity_key,team,is_default)
  VALUES ('cert:issuer-a:01',982001,1),('cert:issuer-a:02',982001,1),
         ('cert:issuer-a:04',982001,0);
INSERT INTO kb_project_membership(identity_key,project)
  VALUES ('cert:issuer-a:01',982010),('cert:issuer-a:02',982010);
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,cert_issuer,cert_serial_norm,authority_id)
  VALUES ('p2b','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','01',
          'active','issuer-a','01','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'),
         ('p2b','dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd','02',
          'active','issuer-a','02','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb');

SELECT org_catalog_bedrock_upsert('p2b-model','P2b Model','converse','anthropic',
  'foundation','aws','us-east-1',NULL,ARRAY['us-east-1'],NULL,'',true);
SELECT org_model_entitle('p2b-model',982001);
SELECT org_pricing_add_version('bedrock','p2b-billable',1.0,2.0,0.5,0.75);
SELECT org_vault_put('team:982001:bedrock',982001,'bedrock','iam',1,
  '\x01'::bytea,'\x0102030405060708090a0b0c'::bytea,'\x02'::bytea,
  '\x0102030405060708090a0b0c0d0e0f10'::bytea);
SELECT org_vault_put('team:982001:bedrock',982001,'bedrock','iam',2,
  '\x03'::bytea,'\x1112131415161718191a1b1c'::bytea,'\x04'::bytea,
  '\x1112131415161718191a1b1c1d1e1f20'::bytea);
INSERT INTO org_vault_rotation(key_id,principal,team_id,agent,cred,from_version,to_version,state)
  VALUES ('p2b-key','team:982001:bedrock',982001,'bedrock','iam',1,2,'activated');
SELECT org_budget_set(982001,NULL,'day',1000.0,NULL);
SELECT org_egress_binding_set(982001,'p2b-model','p2b-billable',1,'p2b-key',
  'team:982001:bedrock','bedrock','iam',1000,100,true);
-- Advance the current price after binding so admission can prove that a stale
-- pinned pointer fails closed; the test restores v1 after the denial.
SELECT org_pricing_add_version('bedrock','p2b-billable',1.1,2.1,0.6,0.8);
INSERT INTO org_rate_policy(dim,scope_key,window_seconds,max_count)
  VALUES ('team','982001',3600,0);

SELECT set_config('aimee.principal','cert:issuer-a:01',true);
DO $$
DECLARE a RECORD; b RECORD; ok BOOLEAN; n BIGINT; st TEXT; spend NUMERIC; reserved NUMERIC;
  spend_before NUMERIC; before_count BIGINT;
BEGIN
  PERFORM kb_enrollment_renew(
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','issuer-a','01','p2b',
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee','issuer-a','03');
  IF NOT EXISTS(SELECT 1 FROM kb_enrollments WHERE cert_issuer='issuer-a' AND
      cert_serial_norm='03' AND authority_id='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb') OR
     NOT EXISTS(SELECT 1 FROM kb_project_membership WHERE project=982010 AND
      identity_key='cert:issuer-a:03') THEN
    RAISE EXCEPTION 'P2b FAIL: authorized renewal did not clone lineage grants';
  END IF;
  BEGIN
    PERFORM kb_enrollment_renew(
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','issuer-a','01','p2b',
      'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff','issuer-a','04');
    RAISE EXCEPTION 'P2b FAIL: conflicting renewal grant was ignored';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
  IF EXISTS(SELECT 1 FROM kb_enrollments WHERE fingerprint=
      'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff') THEN
    RAISE EXCEPTION 'P2b FAIL: conflicting renewal partially committed';
  END IF;
  PERFORM set_config('aimee.principal','cert:issuer-a:02',true);
  BEGIN
    PERFORM kb_enrollment_renew(
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','issuer-a','01','p2b',
      '9999999999999999999999999999999999999999999999999999999999999999','issuer-a','05');
    RAISE EXCEPTION 'P2b FAIL: non-owner renewal accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  PERFORM set_config('aimee.principal','cert:issuer-a:01',true);

  -- A binding is usable only while both its price and activated custody
  -- rotation remain current.
  BEGIN
    PERFORM * FROM org_egress_admit(
      'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'issuer-a','01','cert:issuer-a:01','77777777-7777-4777-8777-777777777777',
      982001,982010,'p2b-model',
      '7777777777777777777777777777777777777777777777777777777777777777',60);
    RAISE EXCEPTION 'P2b FAIL: stale pricing pointer admitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  UPDATE org_model_pricing_current SET version=1 WHERE billable_model='p2b-billable';
  UPDATE org_vault_current SET version=1
    WHERE principal='team:982001:bedrock' AND agent='bedrock' AND cred='iam';
  BEGIN
    PERFORM * FROM org_egress_admit(
      'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'issuer-a','01','cert:issuer-a:01','88888888-8888-4888-8888-888888888888',
      982001,982010,'p2b-model',
      '8888888888888888888888888888888888888888888888888888888888888888',60);
    RAISE EXCEPTION 'P2b FAIL: stale custody rotation admitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  UPDATE org_vault_current SET version=2
    WHERE principal='team:982001:bedrock' AND agent='bedrock' AND cred='iam';

  -- Refusal outcomes must leave every admission mutation rolled back.  The
  -- first call refuses on rate; the second passes rate but refuses on budget.
  SELECT count(*) INTO before_count FROM org_rate_window;
  SELECT * INTO b FROM org_egress_admit(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'issuer-a','01','cert:issuer-a:01','99999999-9999-4999-8999-999999999999',
    982001,982010,'p2b-model',
    '9999999999999999999999999999999999999999999999999999999999999999',60);
  IF b.outcome<>'rate_refused' OR (SELECT count(*) FROM org_rate_window)<>before_count OR
     EXISTS(SELECT 1 FROM org_egress_dispatch WHERE request_id=
       '99999999-9999-4999-8999-999999999999') OR
     EXISTS(SELECT 1 FROM org_budget_reservation WHERE request_id=
       '99999999-9999-4999-8999-999999999999') OR
     EXISTS(SELECT 1 FROM org_token_audit WHERE request_id=
       '99999999-9999-4999-8999-999999999999') THEN
    RAISE EXCEPTION 'P2b FAIL: rate refusal consumed admission state';
  END IF;
  UPDATE org_rate_policy SET max_count=100 WHERE dim='team' AND scope_key='982001';
  UPDATE org_budget SET limit_usd=0 WHERE team_id=982001 AND project_id IS NULL;
  SELECT * INTO b FROM org_egress_admit(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'issuer-a','01','cert:issuer-a:01','aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa',
    982001,982010,'p2b-model',
    'abababababababababababababababababababababababababababababababab',60);
  IF b.outcome<>'budget_refused' OR (SELECT count(*) FROM org_rate_window)<>before_count OR
     EXISTS(SELECT 1 FROM org_budget_counter WHERE team_id=982001) OR
     EXISTS(SELECT 1 FROM org_egress_dispatch WHERE request_id=
       'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa') OR
     EXISTS(SELECT 1 FROM org_budget_reservation WHERE request_id=
       'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa') OR
     EXISTS(SELECT 1 FROM org_token_audit WHERE request_id=
       'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa') THEN
    RAISE EXCEPTION 'P2b FAIL: budget refusal consumed admission state';
  END IF;
  UPDATE org_budget SET limit_usd=1000 WHERE team_id=982001 AND project_id IS NULL;

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
  PERFORM set_config('aimee.principal','cert:issuer-a:02',true);
  SELECT * INTO b FROM org_egress_admit(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',
    'issuer-a','02','cert:issuer-a:02','11111111-1111-4111-8111-111111111111',
    982001,982010,'p2b-model',
    'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',60);
  IF b.outcome <> 'replay' OR b.dispatch_id <> a.dispatch_id THEN
    RAISE EXCEPTION 'P2b FAIL: renewed lineage replay did not return first durable row';
  END IF;
  PERFORM set_config('aimee.principal','cert:issuer-a:01',true);
  BEGIN
    PERFORM * FROM org_egress_admit(
      'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'issuer-a','01','cert:issuer-a:02','44444444-4444-4444-8444-444444444444',
      982001,982010,'p2b-model',
      'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',60);
    RAISE EXCEPTION 'P2b FAIL: noncanonical origin accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  BEGIN
    PERFORM * FROM org_egress_admit(
      'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
      'issuer-a','01','cert:issuer-a:01','55555555-5555-4555-8555-555555555555',
      982001,982011,'p2b-model',
      'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',60);
    RAISE EXCEPTION 'P2b FAIL: nonmember project attribution accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
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
  IF NOT org_egress_dispatch_owner_guard(a.dispatch_id,
    'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',1) OR
     org_egress_dispatch_owner_guard(a.dispatch_id,
    'ffffffffffffffffffffffffffffffff',1) THEN
    RAISE EXCEPTION 'P2b FAIL: ownership write fence validation';
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
  IF a.outcome <> 'admitted' THEN
    RAISE EXCEPTION 'P2b FAIL: stale recovery fixture admission=%',a.outcome;
  END IF;
  UPDATE org_egress_dispatch SET lease_expires_at=now()-interval '1 minute' WHERE id=a.dispatch_id;
  n := org_egress_recover(100);
  IF n < 1 OR (SELECT state FROM org_egress_dispatch WHERE id=a.dispatch_id) <> 'failed' THEN
    RAISE EXCEPTION 'P2b FAIL: stale admitted recovery';
  END IF;

  -- Authenticated usage above the admission ceiling charges the complete
  -- liability, records the overage, and fences this binding from new work.
  SELECT spend_usd INTO spend_before FROM org_budget_counter
    WHERE team_id=982001 AND project_id IS NULL AND period='day';
  SELECT * INTO a FROM org_egress_admit(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'issuer-a','01','cert:issuer-a:01','66666666-6666-4666-8666-666666666666',
    982001,982010,'p2b-model',
    '6666666666666666666666666666666666666666666666666666666666666666',60);
  SELECT * INTO b FROM org_egress_dispatch_begin(
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','66666666-6666-4666-8666-666666666666',
    'abababababababababababababababab','overage-instance',60);
  ok := org_egress_dispatch_settle(a.dispatch_id,
    'abababababababababababababababab',b.owner_generation,
    'succeeded',200,100000000,0,0,0,'complete');
  IF NOT ok OR NOT (SELECT overage_fenced FROM org_egress_binding
      WHERE team_id=982001 AND model_id='p2b-model') OR
     (SELECT overage_usd FROM org_egress_dispatch WHERE id=a.dispatch_id) <= 0 OR
     (SELECT raw_prompt_tokens FROM org_egress_dispatch WHERE id=a.dispatch_id) <> '100000000'
  THEN
    RAISE EXCEPTION 'P2b FAIL: overage evidence/fence missing';
  END IF;
  SELECT spend_usd,reserved_usd INTO spend,reserved FROM org_budget_counter
    WHERE team_id=982001 AND project_id IS NULL AND period='day';
  IF spend-spend_before <> 100 OR reserved <> 0 THEN
    RAISE EXCEPTION 'P2b FAIL: full overage liability spend delta=% reserved=%',
      spend-spend_before,reserved;
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
