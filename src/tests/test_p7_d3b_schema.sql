\set ON_ERROR_STOP on

DO $$
DECLARE n BIGINT;
BEGIN
  SELECT count(*) INTO n FROM information_schema.tables
   WHERE table_schema='public' AND table_name='kb_vault_open_event';
  IF n<>1 THEN RAISE EXCEPTION 'missing D3b open event'; END IF;
END $$;

-- START reserves every mandatory remaining increment before any durable write.
-- Exhausted epoch and fence cases must leave control, operations, and WORM empty.
BEGIN;
UPDATE public.kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id='',
  seal_epoch=9223372036854775806,fencing_token=1 WHERE singleton=1;
SET ROLE aimee_kb_vault_orchestrator;
DO $$ BEGIN
  PERFORM aimee_kb_vault_orchestrator_api.org_vault_rewrap_reserve(
    'uid:0','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',0,1);
  RAISE EXCEPTION 'epoch-exhausted reservation accepted';
EXCEPTION WHEN numeric_value_out_of_range THEN NULL; END $$;
RESET ROLE;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_vault_rewrap_operation
             WHERE request_id='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa') OR
     EXISTS(SELECT 1 FROM public.kb_vault_rewrap_worm
             WHERE operation_id='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb') OR
     (SELECT sealed OR seal_epoch<>9223372036854775806 OR fencing_token<>1
        FROM public.kb_vault_control WHERE singleton=1) THEN
    RAISE EXCEPTION 'epoch exhaustion had durable effects';
  END IF;
END $$;
ROLLBACK;

BEGIN;
UPDATE public.kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id='',
  seal_epoch=1,fencing_token=9223372036854775805 WHERE singleton=1;
SET ROLE aimee_kb_vault_orchestrator;
DO $$ BEGIN
  PERFORM aimee_kb_vault_orchestrator_api.org_vault_rewrap_reserve(
    'uid:0','cccccccccccccccccccccccccccccccc','dddddddddddddddddddddddddddddddd',0,1);
  RAISE EXCEPTION 'fence-exhausted reservation accepted';
EXCEPTION WHEN numeric_value_out_of_range THEN NULL; END $$;
RESET ROLE;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_vault_rewrap_operation
             WHERE request_id='cccccccccccccccccccccccccccccccc') OR
     EXISTS(SELECT 1 FROM public.kb_vault_rewrap_worm
             WHERE operation_id='dddddddddddddddddddddddddddddddd') OR
     (SELECT sealed OR seal_epoch<>1 OR fencing_token<>9223372036854775805
        FROM public.kb_vault_control WHERE singleton=1) THEN
    RAISE EXCEPTION 'fence exhaustion had durable effects';
  END IF;
END $$;
ROLLBACK;

SET ROLE aimee_kb_vault_orchestrator_login;
DO $$ BEGIN
  PERFORM aimee_kb_vault_orchestrator_api.org_vault_rewrap_operator_status();
  RAISE EXCEPTION 'login unexpectedly invoked status';
EXCEPTION WHEN insufficient_privilege THEN NULL; END $$;
SET ROLE aimee_kb_vault_orchestrator;

DO $$
DECLARE r RECORD;
BEGIN
  SELECT * INTO STRICT r FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_reserve(
    'uid:0','00112233445566778899aabbccddeeff','11111111111111111111111111111111',0,1);
  IF NOT r.created OR r.operation_id<>'11111111111111111111111111111111' OR
     r.actor<>'uid:0' OR r.state<>'preparing' OR r.old_generation<>0 OR r.new_generation<>1 THEN
    RAISE EXCEPTION 'bad initial reservation';
  END IF;
  SELECT * INTO STRICT r FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_reserve(
    'uid:0','00112233445566778899aabbccddeeff','22222222222222222222222222222222',0,1);
  IF r.created OR r.operation_id<>'11111111111111111111111111111111' THEN
    RAISE EXCEPTION 'candidate leaked into replay';
  END IF;
  BEGIN
    PERFORM aimee_kb_vault_orchestrator_api.org_vault_rewrap_reserve(
      'uid:0','00112233445566778899aabbccddeeff','22222222222222222222222222222222',1,2);
    RAISE EXCEPTION 'changed replay accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  SELECT * INTO STRICT r FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_active();
  IF r.operation_id<>'11111111111111111111111111111111' THEN
    RAISE EXCEPTION 'active discovery mismatch';
  END IF;
END $$;
RESET ROLE;

-- Simulate the durable D2 completion boundary, then prove RESUME can derive
-- correlation solely from the status-selected operation across transactions.
UPDATE public.kb_vault_rewrap_operation SET state='completed',
  receipt=decode(repeat('ab',208),'hex'),
  receipt_digest=sha256(decode(repeat('ab',208),'hex')),
  inventory_digest=decode(repeat('cd',32),'hex'),stage_digest=decode(repeat('ef',32),'hex');
UPDATE public.kb_vault_control SET sealed=true,maintenance_kind='',maintenance_id='',
  fencing_token=3 WHERE singleton=1;
SET ROLE aimee_kb_vault_orchestrator;
DO $$ BEGIN
  PERFORM aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed(
    'uid:0','00112233445566778899aabbccddeeff','11111111111111111111111111111111');
  RAISE EXCEPTION 'completed material accepted missing checkpoint';
EXCEPTION WHEN SQLSTATE 'P7I01' THEN NULL; END $$;
RESET ROLE;
SELECT public.org_vault_rewrap_worm_append(
  '11111111111111111111111111111111','completed','completed','');
DO $$ BEGIN IF (SELECT count(*) FROM public.kb_vault_rewrap_worm
 WHERE operation_id='11111111111111111111111111111111' AND event_kind='completed')<>1
 THEN RAISE EXCEPTION 'checkpoint test seed missing'; END IF; END $$;
SET ROLE aimee_kb_vault_orchestrator;
DO $$
DECLARE r RECORD;
BEGIN
  SELECT * INTO STRICT r FROM
    aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed_active(
      'uid:0','11111111111111111111111111111111');
  IF r.request_id<>'00112233445566778899aabbccddeeff' OR
     r.operation_id<>'11111111111111111111111111111111' OR
     r.fencing_token<>2 OR octet_length(r.receipt)<>208 THEN
    RAISE EXCEPTION 'completed-active material mismatch';
  END IF;
END $$;
RESET ROLE;
-- A second transaction is the restart/reconnect replay posture.
SET ROLE aimee_kb_vault_orchestrator;
SELECT count(*) FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed_active(
  'uid:0','11111111111111111111111111111111') HAVING count(*)=1;
RESET ROLE;

BEGIN;
INSERT INTO public.kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,
 fencing_token,old_generation,new_generation,failure_class)
VALUES('33333333333333333333333333333333','33333333333333333333333333333333','uid:0',
 'aborted',4,4,1,2,'test_abort');
SET ROLE aimee_kb_vault_orchestrator;
DO $$ BEGIN
  PERFORM aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed_active(
    'uid:0','11111111111111111111111111111111');
  RAISE EXCEPTION 'completed-active hid a later row';
EXCEPTION WHEN SQLSTATE 'P7I01' THEN NULL; END $$;
RESET ROLE;
ROLLBACK;

-- The open transaction itself (not only the earlier material read) re-proves
-- the exact sole obligation and immutable completed checkpoint.
BEGIN;
SET ROLE aimee_kb_vault_orchestrator;
DO $$
DECLARE o RECORD; r RECORD; e RECORD;
BEGIN
  SELECT * INTO STRICT o FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed_active(
    'uid:0','11111111111111111111111111111111');
  SELECT * INTO STRICT r FROM aimee_kb_vault_orchestrator_api.org_vault_rewrap_open_completed(
    'uid:0',o.request_id,o.operation_id,o.seal_epoch,o.fencing_token,
    o.receipt_digest,o.inventory_digest,o.stage_digest);
  SELECT * INTO STRICT e FROM aimee_kb_vault_orchestrator_api.org_vault_open_event(r.event_id);
  IF r.opened_epoch<>o.seal_epoch+1 OR e.row_hash<>r.row_hash OR
     e.operation_id<>o.operation_id OR e.event_kind<>'completed_opened' THEN
    RAISE EXCEPTION 'completed open result mismatch';
  END IF;
END $$;
RESET ROLE;
DO $$
BEGIN
  IF (SELECT count(*) FROM public.kb_audit_event
       WHERE action='vault.rewrap.open.completed')<>1 THEN
    RAISE EXCEPTION 'completed open primary audit mismatch';
  END IF;
END $$;
ROLLBACK;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_audit_event
             WHERE action='vault.rewrap.open.completed') THEN
    RAISE EXCEPTION 'completed open audit escaped rollback';
  END IF;
END $$;

BEGIN;
INSERT INTO public.kb_vault_rewrap_operation(operation_id,request_id,actor,state,seal_epoch,
 fencing_token,old_generation,new_generation,failure_class,failure_from_state)
VALUES('44444444444444444444444444444444','44444444444444444444444444444444','uid:0',
 'recovery_required',1,1,9,10,'test_recovery','preparing');
SET ROLE aimee_kb_vault_orchestrator;
DO $$ BEGIN
  PERFORM aimee_kb_vault_orchestrator_api.org_vault_rewrap_completed(
    'uid:0','00112233445566778899aabbccddeeff','11111111111111111111111111111111');
  RAISE EXCEPTION 'completed material accepted multiple obligations';
EXCEPTION WHEN SQLSTATE 'P7I01' THEN NULL; END $$;
RESET ROLE;
ROLLBACK;

-- Exercise idle-open atomicity and exact replay in an isolated rollback scope.
BEGIN;
UPDATE public.kb_vault_rewrap_operation SET state='aborted',failure_class='test_abort';
UPDATE public.kb_vault_control SET sealed=true,maintenance_kind='',maintenance_id='',
  seal_epoch=10,fencing_token=10,last_opened_rewrap_fence=0;
SET ROLE aimee_kb_vault_orchestrator;
DO $$
DECLARE r RECORD; e RECORD;
BEGIN
  SELECT * INTO STRICT r FROM aimee_kb_vault_orchestrator_api.org_vault_open_idle(
    'uid:0','ffeeddccbbaa99887766554433221100',10,10,0);
  IF r.opened_epoch<>11 OR r.opened_fence<>11 OR octet_length(r.row_hash)<>32 THEN
    RAISE EXCEPTION 'idle open result mismatch';
  END IF;
  SELECT * INTO STRICT e FROM aimee_kb_vault_orchestrator_api.org_vault_open_event(r.event_id);
  IF e.row_hash<>r.row_hash OR e.actor<>'uid:0' OR e.event_kind<>'idle_opened' THEN
    RAISE EXCEPTION 'open event readback mismatch';
  END IF;
END $$;
RESET ROLE;
SELECT public.kb_audit_worm_drain(1000);
DO $$
BEGIN
  IF (SELECT count(*) FROM public.kb_audit_event
       WHERE action='vault.rewrap.open.idle')<>1 THEN
    RAISE EXCEPTION 'idle open primary audit mismatch';
  END IF;
  PERFORM public.kb_audit_worm_append('test','test','unrelated.audit','test','allow','');
  PERFORM public.kb_audit_worm_drain(1000);
END $$;
SET ROLE aimee_kb_vault_orchestrator;
DO $$
DECLARE r RECORD; e RECORD;
BEGIN
  SELECT * INTO STRICT r FROM aimee_kb_vault_orchestrator_api.org_vault_open_idle(
    'uid:0','ffeeddccbbaa99887766554433221100',10,10,0);
  SELECT * INTO STRICT e FROM aimee_kb_vault_orchestrator_api.org_vault_open_event(r.event_id);
  IF e.event_id<>r.event_id OR e.row_hash<>r.row_hash THEN
    RAISE EXCEPTION 'idle open replay mismatch after audit head advance';
  END IF;
END $$;
RESET ROLE;
DO $$ BEGIN
  IF (SELECT count(*) FROM public.kb_audit_event
       WHERE action='vault.rewrap.open.idle')<>1 THEN
    RAISE EXCEPTION 'idle open replay duplicated primary audit';
  END IF;
END $$;
ROLLBACK;
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM public.kb_audit_event
             WHERE action IN ('vault.rewrap.open.idle','unrelated.audit')) THEN
    RAISE EXCEPTION 'idle open audit escaped rollback';
  END IF;
END $$;

DO $$
DECLARE bad BIGINT; public_usage BOOLEAN;
BEGIN
  SELECT has_schema_privilege('aimee_kb_vault_orchestrator','public','USAGE') INTO public_usage;
  IF public_usage THEN RAISE EXCEPTION 'capability has public schema usage'; END IF;
  SELECT count(*) INTO bad FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
   WHERE left(n.nspname,3)<>'pg_' AND
         n.nspname NOT IN ('information_schema','aimee_kb_vault_orchestrator_api') AND
         has_schema_privilege('aimee_kb_vault_orchestrator',n.oid,'USAGE') AND
         has_function_privilege('aimee_kb_vault_orchestrator',p.oid,'EXECUTE');
  IF bad<>0 THEN RAISE EXCEPTION 'capability invokes unrelated functions: %',bad; END IF;
END $$;
