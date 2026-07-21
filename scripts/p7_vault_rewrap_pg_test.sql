-- P7-reseal-c owner-only inventory, staging, fencing, WORM, and promotion gate.
\set ON_ERROR_STOP on

-- The schema is provisioned as the owner.  These helpers deliberately exist only
-- inside this throwaway gate database and are removed at the end.
CREATE OR REPLACE FUNCTION p7_rewrap_expect(p_sql TEXT,p_state TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$
DECLARE got TEXT;
BEGIN
  EXECUTE p_sql INTO got;
  IF got IS DISTINCT FROM p_state THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: expected state %, got % from %',p_state,got,p_sql;
  END IF;
END; $$;

CREATE OR REPLACE FUNCTION p7_rewrap_expect_error(p_sql TEXT,p_sqlstate TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$
BEGIN
  BEGIN
    EXECUTE p_sql;
    RAISE EXCEPTION 'P7 rewrap FAIL: statement unexpectedly succeeded: %',p_sql;
  EXCEPTION WHEN OTHERS THEN
    IF SQLSTATE='P0001' AND SQLERRM LIKE 'P7 rewrap FAIL:%' THEN RAISE; END IF;
    IF SQLSTATE<>p_sqlstate THEN
      RAISE EXCEPTION 'P7 rewrap FAIL: expected SQLSTATE %, got % (%) for %',
        p_sqlstate,SQLSTATE,SQLERRM,p_sql;
    END IF;
  END;
END; $$;

CREATE OR REPLACE FUNCTION p7_rewrap_stage_all(p_op TEXT,p_fence BIGINT) RETURNS VOID
LANGUAGE plpgsql AS $$
DECLARE r RECORD; next_wrap BYTEA; next_check BYTEA;
BEGIN
  FOR r IN SELECT * FROM org_vault_rewrap_secret_page(p_op,p_fence,0,128) LOOP
    -- Include the source digest so consecutive promoted test rotations never
    -- attempt to stage a byte-identical wrap for the same stable source ID.
    next_wrap:=sha256(r.source_digest||decode('a1','hex'))||
      substring(sha256(r.source_digest||decode('a2','hex')) FROM 1 FOR 8);
    PERFORM org_vault_rewrap_stage_dek(p_op,p_fence,r.source_id,r.principal,r.agent,r.cred,
      r.version,r.source_digest,next_wrap);
  END LOOP;
  FOR r IN SELECT * FROM org_vault_rewrap_check_page(p_op,p_fence,'',128) LOOP
    next_check:=CASE WHEN octet_length(r.kek_check)=0 THEN ''::bytea ELSE
      sha256(r.source_digest||convert_to(r.principal,'UTF8')||decode('b1','hex'))||
      substring(sha256(r.source_digest||convert_to(r.principal,'UTF8')||decode('b2','hex'))
        FROM 1 FOR 8) END;
    PERFORM org_vault_rewrap_stage_check(p_op,p_fence,r.principal,r.source_digest,next_check);
  END LOOP;
END; $$;

-- Owner-only surface: broad grant reapplication must not expose tables, helpers,
-- inventory pages, or transitions to PUBLIC or the runtime role.
DO $$
DECLARE tables TEXT[]:=ARRAY['kb_vault_rewrap_check_stage','kb_vault_rewrap_dek_stage',
  'kb_vault_rewrap_operation','kb_vault_rewrap_worm'];
DECLARE funcs TEXT[]:=ARRAY['org_vault_rewrap_abort','org_vault_rewrap_assert_live',
  'org_vault_rewrap_begin','org_vault_rewrap_check_page','org_vault_rewrap_digests',
  'org_vault_rewrap_complete',
  'org_vault_rewrap_mark_committing','org_vault_rewrap_mark_resealed',
  'org_vault_rewrap_pack_bytes','org_vault_rewrap_pack_text','org_vault_rewrap_promote',
  'org_vault_rewrap_record_prepared','org_vault_rewrap_recovery_required',
  'org_vault_rewrap_secret_page','org_vault_rewrap_snapshot','org_vault_rewrap_stage_check',
  'org_vault_rewrap_stage_dek','org_vault_rewrap_stage_finish','org_vault_rewrap_status',
  'org_vault_rewrap_verify_check_page','org_vault_rewrap_verify_secret_page',
  'org_vault_rewrap_verify_summary',
  'org_vault_rewrap_worm_append','org_vault_rewrap_worm_block'];
DECLARE d2funcs TEXT[]:=ARRAY['org_vault_rewrap_snapshot','org_vault_rewrap_verify_check_page',
  'org_vault_rewrap_verify_secret_page','org_vault_rewrap_verify_summary'];
DECLARE d2sigs TEXT[]:=ARRAY['org_vault_rewrap_snapshot(text)',
  'org_vault_rewrap_verify_check_page(text, bigint, bytea, integer)',
  'org_vault_rewrap_verify_secret_page(text, bigint, bigint, integer)',
  'org_vault_rewrap_verify_summary(text, bigint)'];
DECLARE f RECORD;
BEGIN
  IF (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
      WHERE n.nspname='public' AND c.relname=ANY(tables) AND c.relkind IN ('r','p'))<>
      cardinality(tables) OR
     NOT EXISTS(SELECT 1 FROM pg_index i JOIN pg_class x ON x.oid=i.indexrelid
       JOIN pg_namespace n ON n.oid=x.relnamespace WHERE n.nspname='public' AND
       x.relname='idx_kb_vault_rewrap_one_active' AND i.indisunique AND
       pg_get_expr(i.indpred,i.indrelid) ~ 'aborted' AND
       pg_get_expr(i.indpred,i.indrelid) ~ 'recovery_required' AND
       pg_get_expr(i.indpred,i.indrelid) ~ 'completed') THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: table/active-index inventory drift';
  END IF;
  IF NOT EXISTS(SELECT 1 FROM information_schema.columns WHERE table_schema='public' AND
       table_name='kb_vault_rewrap_operation' AND column_name='failure_from_state') OR
     NOT EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='kb_vault_rewrap_operation'::regclass
       AND conname='kb_vault_rewrap_operation_state_check' AND
       pg_get_constraintdef(oid) ~ 'completed') OR
     NOT EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='kb_vault_rewrap_worm'::regclass
       AND conname='kb_vault_rewrap_worm_event_kind_check' AND
       pg_get_constraintdef(oid) ~ 'completed') THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: D1 state/outbox schema drift';
  END IF;
  IF (SELECT count(*) FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid
      WHERE c.relname='kb_vault_rewrap_worm' AND NOT t.tgisinternal AND
        t.tgname IN ('kb_vault_rewrap_worm_no_update','kb_vault_rewrap_worm_no_delete',
                     'kb_vault_rewrap_worm_no_truncate'))<>3 THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: WORM trigger inventory drift';
  END IF;
  IF EXISTS(SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
      CROSS JOIN LATERAL aclexplode(COALESCE(c.relacl,acldefault('r',c.relowner))) a
      WHERE n.nspname='public' AND c.relname=ANY(tables) AND
        (a.grantee=0 OR a.grantee='aimee_kb_runtime'::regrole) AND
        a.privilege_type IN ('SELECT','INSERT','UPDATE','DELETE','TRUNCATE')) THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: owner table privilege leaked';
  END IF;
  IF EXISTS(SELECT 1 FROM unnest(tables) t(name)
      WHERE has_table_privilege('aimee_kb_runtime',format('public.%I',name),
                                'SELECT,INSERT,UPDATE,DELETE,TRUNCATE')) THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: runtime has effective table privilege through membership';
  END IF;
  IF EXISTS(SELECT 1 FROM pg_namespace n
      CROSS JOIN LATERAL aclexplode(COALESCE(n.nspacl,acldefault('n',n.nspowner))) a
      WHERE n.nspname='public' AND
        (a.grantee=0 OR a.grantee='aimee_kb_runtime'::regrole) AND a.privilege_type='CREATE') THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: public schema shadowing privilege leaked';
  END IF;
  FOR f IN SELECT p.oid,p.proname,p.prosecdef,p.provolatile,p.proconfig
      FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname=ANY(funcs) LOOP
    IF EXISTS(SELECT 1 FROM aclexplode(COALESCE(
         (SELECT proacl FROM pg_proc WHERE oid=f.oid),
         acldefault('f',(SELECT proowner FROM pg_proc WHERE oid=f.oid)))) a
       WHERE (a.grantee=0 OR a.grantee='aimee_kb_runtime'::regrole) AND
             a.privilege_type='EXECUTE') THEN
      RAISE EXCEPTION 'P7 rewrap FAIL: execute privilege leaked on %',f.proname;
    END IF;
    IF has_function_privilege('aimee_kb_runtime',f.oid,'EXECUTE') THEN
      RAISE EXCEPTION 'P7 rewrap FAIL: runtime has effective execute through membership on %',
        f.proname;
    END IF;
    IF (f.proname=ANY(d2funcs) AND
          NOT f.proconfig @> ARRAY['search_path=pg_catalog']::TEXT[]) OR
       (NOT (f.proname=ANY(d2funcs)) AND
          NOT f.proconfig @> ARRAY['search_path=pg_catalog, public, pg_temp']::TEXT[]) OR
       (f.proname NOT IN ('org_vault_rewrap_pack_bytes','org_vault_rewrap_pack_text',
                          'org_vault_rewrap_worm_block') AND
        (NOT f.prosecdef OR f.provolatile<>'v')) THEN
      RAISE EXCEPTION 'P7 rewrap FAIL: security attributes drift on %',f.proname;
    END IF;
  END LOOP;
  IF (SELECT count(DISTINCT p.proname) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname=ANY(funcs))<>cardinality(funcs) THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: owner function inventory incomplete';
  END IF;
  IF EXISTS(SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
      WHERE n.nspname='public' AND p.proname=ANY(d2funcs) AND
        (p.proname||'('||pg_catalog.oidvectortypes(p.proargtypes)||')')<>ALL(d2sigs)) OR
     (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
       WHERE n.nspname='public' AND p.proname=ANY(d2funcs))<>cardinality(d2sigs) THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: D2a exact overload inventory drift';
  END IF;
END $$;

-- Independent known-answer vectors pin network-order byte lengths, UTF-8 byte
-- counting (not character counting), embedded delimiters/NUL-like bytes, and the
-- WORM domain preimage without reusing the helper to derive the expected bytes.
DO $$ BEGIN
  IF encode(org_vault_rewrap_pack_text('π|x'),'hex')<>'00000004cf807c78' OR
     encode(org_vault_rewrap_pack_text(''),'hex')<>'00000000' OR
     encode(org_vault_rewrap_pack_bytes('\x00ff7c'),'hex')<>'0000000300ff7c' THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: canonical packing known-answer mismatch';
  END IF;
END $$;

-- Build a small but nontrivial inventory: historical versions, delimiter-like and
-- non-ASCII identities, and a salt principal with no secret and an empty check.
BEGIN;
SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES(970722,'p7_rewrap');
INSERT INTO kb_team_membership(identity_key,team,is_default) VALUES('owner',970722,0);
SELECT org_vault_salt_ensure('team:970722:provider:bedrock','\x01');
SELECT org_vault_kek_check_set('team:970722:provider:bedrock',decode(repeat('21',40),'hex'));
SELECT org_vault_salt_ensure('unused:π|principal','\x02');
SELECT org_vault_put('team:970722:provider:bedrock',970722,'bed|rock','primary',1,
  decode(repeat('11',40),'hex'),decode(repeat('12',12),'hex'),'\x13',decode(repeat('14',16),'hex'));
SELECT org_vault_put('team:970722:provider:bedrock',970722,'bed|rock','primary',2,
  decode(repeat('15',40),'hex'),decode(repeat('16',12),'hex'),'\x17',decode(repeat('18',16),'hex'));
-- Earlier P7 gates deliberately leave crash-resumable rotation fixtures behind.
-- Retire only those throwaway fixtures so reseal admission starts from its
-- required clean-rotation precondition in this shared gate database.
UPDATE org_vault_rotation SET state='retired',updated_at=pg_now_text() WHERE state<>'retired';
COMMIT;

-- An owner-only inventory page still requires SERIALIZABLE staging semantics.
SELECT p7_rewrap_expect_error($q$SELECT * FROM org_vault_rewrap_secret_page(
  '00000000000000000000000000000000',1,0,1)$q$,'25001');

-- Abort path: exact begin and terminal replays work after the fence advances;
-- conflicting replays and stale writers remain typed conflicts.
BEGIN;
SELECT set_config('aimee.p7_abort_fence',fencing_token::text,false)
  FROM org_vault_rewrap_begin('owner','request-abort','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',10,11);
SELECT p7_rewrap_expect($q$SELECT state FROM org_vault_rewrap_begin(
  'owner','request-abort','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',10,11)$q$,'preparing');
SELECT p7_rewrap_expect_error($q$SELECT * FROM org_vault_rewrap_begin(
  'owner','request-abort','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',10,11)$q$,'23505');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_abort(
  'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',current_setting('aimee.p7_abort_fence')::bigint,
  'raw exception: secret=do-not-persist')$q$,'22023');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_abort(
  'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',current_setting('aimee.p7_abort_fence')::bigint,'operator_cancel')$q$,
  'aborted');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_abort(
  'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',current_setting('aimee.p7_abort_fence')::bigint,'different')$q$,
  '23505');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_abort(
  'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',current_setting('aimee.p7_abort_fence')::bigint,'operator_cancel')$q$,
  'aborted');
DO $$ BEGIN
  IF (SELECT state FROM kb_vault_rewrap_operation WHERE request_id='request-abort')<>'aborted' OR
     (SELECT sealed OR maintenance_kind<>'' OR maintenance_id<>'' FROM kb_vault_control) OR
     (SELECT count(*) FROM kb_vault_rewrap_worm WHERE operation_id=
       'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' AND event_kind IN ('intent','abort'))<>2 THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: abort state/barrier/WORM mismatch';
  END IF;
END $$;
COMMIT;

-- A lost response from an earlier prepared/stage-finish transition remains an
-- exact replay after an abort that terminalized a fully staged operation.
BEGIN;
SELECT set_config('aimee.p7_abort_staged_fence',fencing_token::text,false)
  FROM org_vault_rewrap_begin('owner','request-abort-staged',
    'dddddddddddddddddddddddddddddddd',11,12);
SELECT org_vault_rewrap_record_prepared('dddddddddddddddddddddddddddddddd',
  current_setting('aimee.p7_abort_staged_fence')::bigint,'\x737461676564',
  sha256('\x737461676564'::bytea));
COMMIT;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT p7_rewrap_stage_all('dddddddddddddddddddddddddddddddd',
  current_setting('aimee.p7_abort_staged_fence')::bigint);
SELECT org_vault_rewrap_stage_finish('dddddddddddddddddddddddddddddddd',
  current_setting('aimee.p7_abort_staged_fence')::bigint);
COMMIT;
BEGIN;
SELECT org_vault_rewrap_abort('dddddddddddddddddddddddddddddddd',
  current_setting('aimee.p7_abort_staged_fence')::bigint,'staged_cancel');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_record_prepared(
  'dddddddddddddddddddddddddddddddd',current_setting('aimee.p7_abort_staged_fence')::bigint,
  '\x737461676564',sha256('\x737461676564'::bytea))$q$,'aborted');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_stage_finish(
  'dddddddddddddddddddddddddddddddd',
  current_setting('aimee.p7_abort_staged_fence')::bigint)$q$,'aborted');
COMMIT;

-- D1 quarantine is legal from every active phase.  Build each phase through the
-- owner API, then prove evidence preservation, source provenance, terminal replay,
-- outbox exactness, fence behavior, and fail-closed outgoing edges.
CREATE OR REPLACE FUNCTION p7_rewrap_recovery_case(p_op TEXT,p_request TEXT,
  p_target TEXT,p_old BIGINT) RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE f BIGINT; before_op kb_vault_rewrap_operation%ROWTYPE;
  after_op kb_vault_rewrap_operation%ROWTYPE; before_fence BIGINT;
  receipt_bytes BYTEA:=convert_to('receipt-'||p_target,'UTF8');
  dek_rows BIGINT; check_rows BIGINT; expected_event TEXT;
BEGIN
  SELECT fencing_token INTO STRICT f FROM org_vault_rewrap_begin(
    'owner',p_request,p_op,p_old,p_old+1);
  IF p_target<>'preparing' THEN
    PERFORM org_vault_rewrap_record_prepared(p_op,f,receipt_bytes,sha256(receipt_bytes));
  END IF;
  IF p_target IN ('wraps_staged','reseal_committing','resealed','promoted') THEN
    PERFORM p7_rewrap_stage_all(p_op,f);
    PERFORM org_vault_rewrap_stage_finish(p_op,f);
  END IF;
  IF p_target IN ('reseal_committing','resealed','promoted') THEN
    PERFORM org_vault_rewrap_mark_committing(p_op,f);
  END IF;
  IF p_target IN ('resealed','promoted') THEN
    PERFORM org_vault_rewrap_mark_resealed(p_op,f,sha256(receipt_bytes));
  END IF;
  IF p_target='promoted' THEN
    PERFORM org_vault_rewrap_promote(p_op,f);
  END IF;
  SELECT * INTO STRICT before_op FROM kb_vault_rewrap_operation WHERE operation_id=p_op;
  SELECT fencing_token INTO STRICT before_fence FROM kb_vault_control WHERE singleton=1;
  SELECT count(*) INTO dek_rows FROM kb_vault_rewrap_dek_stage WHERE operation_id=p_op;
  SELECT count(*) INTO check_rows FROM kb_vault_rewrap_check_stage WHERE operation_id=p_op;

  IF p_target='preparing' THEN
    PERFORM p7_rewrap_expect_error(format(
      'SELECT org_vault_rewrap_recovery_required(%L,%s,%L)',p_op,f,'Uppercase'),'22023');
    UPDATE kb_vault_rewrap_operation SET fencing_token=9223372036854775807
      WHERE operation_id=p_op;
    UPDATE kb_vault_control SET fencing_token=9223372036854775807 WHERE singleton=1;
    PERFORM p7_rewrap_expect_error(format(
      'SELECT org_vault_rewrap_recovery_required(%L,9223372036854775807,%L)',
      p_op,'fence_exhausted'),'22003');
    UPDATE kb_vault_rewrap_operation SET fencing_token=f WHERE operation_id=p_op;
    UPDATE kb_vault_control SET fencing_token=before_fence WHERE singleton=1;
  END IF;

  IF org_vault_rewrap_recovery_required(p_op,f,'quarantine_test')<>'recovery_required' THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: quarantine failed from %',p_target;
  END IF;
  SELECT * INTO STRICT after_op FROM kb_vault_rewrap_operation WHERE operation_id=p_op;
  expected_event:=encode(sha256(convert_to('aimee-vault-rewrap-worm-v1','UTF8') ||
    org_vault_rewrap_pack_text(p_op) ||
    org_vault_rewrap_pack_text('recovery_required')),'hex');
  IF after_op.state<>'recovery_required' OR after_op.failure_class<>'quarantine_test' OR
     after_op.failure_from_state<>p_target OR
     after_op.receipt IS DISTINCT FROM before_op.receipt OR
     after_op.receipt_digest IS DISTINCT FROM before_op.receipt_digest OR
     after_op.secret_count<>before_op.secret_count OR after_op.check_count<>before_op.check_count OR
     after_op.inventory_digest IS DISTINCT FROM before_op.inventory_digest OR
     after_op.stage_digest IS DISTINCT FROM before_op.stage_digest OR
     (SELECT count(*) FROM kb_vault_rewrap_dek_stage WHERE operation_id=p_op)<>dek_rows OR
     (SELECT count(*) FROM kb_vault_rewrap_check_stage WHERE operation_id=p_op)<>check_rows OR
     NOT (SELECT sealed AND maintenance_kind='tpm2-reseal' AND maintenance_id=p_op AND
       fencing_token=before_fence+1 FROM kb_vault_control WHERE singleton=1) OR
     (SELECT count(*) FROM kb_vault_rewrap_worm w WHERE w.operation_id=p_op AND
       w.event_kind='recovery_required' AND w.event_id=expected_event AND
       w.state='recovery_required' AND w.fencing_token=f AND
       w.receipt_digest IS NOT DISTINCT FROM before_op.receipt_digest AND
       w.inventory_digest IS NOT DISTINCT FROM before_op.inventory_digest AND
       w.stage_digest IS NOT DISTINCT FROM before_op.stage_digest AND
       w.detail='from_state='||p_target||';class=quarantine_test')<>1 THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: quarantine evidence/barrier/outbox mismatch from %',p_target;
  END IF;
  PERFORM p7_rewrap_expect(format(
    'SELECT org_vault_rewrap_recovery_required(%L,%s,%L)',p_op,f,'quarantine_test'),
    'recovery_required');
  PERFORM p7_rewrap_expect_error(format(
    'SELECT org_vault_rewrap_recovery_required(%L,%s,%L)',p_op,f,'different'),'23505');
  PERFORM p7_rewrap_expect_error(format(
    'SELECT org_vault_rewrap_abort(%L,%s,%L)',p_op,f,'no_escape'),'40001');
  PERFORM p7_rewrap_expect_error(format(
    'SELECT org_vault_rewrap_complete(%L,%s,decode(repeat(''00'',32),''hex''),decode(repeat(''00'',32),''hex''),decode(repeat(''00'',32),''hex''))',p_op,f),'P7C01');

  IF p_target='preparing' THEN
    PERFORM p7_rewrap_expect_error(format(
      'SELECT org_vault_rewrap_record_prepared(%L,%s,%L::bytea,sha256(%L::bytea))',
      p_op,f,'\\x78','\\x78'),'40001');
  ELSE
    PERFORM p7_rewrap_expect(format(
      'SELECT org_vault_rewrap_record_prepared(%L,%s,%L::bytea,%L::bytea)',
      p_op,f,before_op.receipt,before_op.receipt_digest),'recovery_required');
  END IF;
  IF p_target IN ('wraps_staged','reseal_committing','resealed','promoted') THEN
    PERFORM p7_rewrap_expect(format('SELECT org_vault_rewrap_stage_finish(%L,%s)',p_op,f),
      'recovery_required');
  ELSE
    PERFORM p7_rewrap_expect_error(format('SELECT org_vault_rewrap_stage_finish(%L,%s)',p_op,f),
      '40001');
  END IF;
  IF p_target IN ('reseal_committing','resealed','promoted') THEN
    PERFORM p7_rewrap_expect(format('SELECT org_vault_rewrap_mark_committing(%L,%s)',p_op,f),
      'recovery_required');
  ELSE
    PERFORM p7_rewrap_expect_error(format('SELECT org_vault_rewrap_mark_committing(%L,%s)',p_op,f),
      '40001');
  END IF;
  IF p_target IN ('resealed','promoted') THEN
    PERFORM p7_rewrap_expect(format(
      'SELECT org_vault_rewrap_mark_resealed(%L,%s,%L::bytea)',p_op,f,before_op.receipt_digest),
      'recovery_required');
  ELSE
    PERFORM p7_rewrap_expect_error(format(
      'SELECT org_vault_rewrap_mark_resealed(%L,%s,decode(repeat(''00'',32),''hex''))',p_op,f),
      '40001');
  END IF;
  IF p_target='promoted' THEN
    PERFORM p7_rewrap_expect(format('SELECT org_vault_rewrap_promote(%L,%s)',p_op,f),
      'recovery_required');
  ELSE
    PERFORM p7_rewrap_expect_error(format('SELECT org_vault_rewrap_promote(%L,%s)',p_op,f),
      '40001');
  END IF;
  UPDATE kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id='',
    updated_at=pg_now_text() WHERE singleton=1;
END; $$;

BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT p7_rewrap_recovery_case('b0000000000000000000000000000001','recovery-preparing','preparing',30);
SELECT p7_rewrap_recovery_case('b0000000000000000000000000000002','recovery-custody','custody_prepared',31);
SELECT p7_rewrap_recovery_case('b0000000000000000000000000000003','recovery-wraps','wraps_staged',32);
SELECT p7_rewrap_recovery_case('b0000000000000000000000000000004','recovery-committing','reseal_committing',33);
SELECT p7_rewrap_recovery_case('b0000000000000000000000000000005','recovery-resealed','resealed',34);
SELECT p7_rewrap_recovery_case('b0000000000000000000000000000006','recovery-promoted','promoted',35);
COMMIT;

-- Main successful operation through staging.  Receipt and stage insertions are
-- exact-idempotent; omitted inventory and stale fences fail without side effects.
BEGIN;
SELECT set_config('aimee.p7_rewrap_fence',fencing_token::text,false),
       set_config('aimee.p7_rewrap_epoch',seal_epoch::text,false)
  FROM org_vault_rewrap_begin('owner','request-promote','cccccccccccccccccccccccccccccccc',11,12);
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_record_prepared(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  '\x72656365697074',decode(repeat('00',32),'hex'))$q$,'22023');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_record_prepared(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  '\x72656365697074',sha256('\x72656365697074'::bytea))$q$,'custody_prepared');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_record_prepared(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  '\x72656365697074',sha256('\x72656365697074'::bytea))$q$,'custody_prepared');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_record_prepared(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint-1,
  '\x78',sha256('\x78'::bytea))$q$,'40001');
COMMIT;

BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_stage_finish(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'40001');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_stage_dek(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint-1,
  1,'x','','',1,decode(repeat('00',32),'hex'),decode(repeat('01',40),'hex'))$q$,'40001');
SELECT p7_rewrap_stage_all('cccccccccccccccccccccccccccccccc',
  current_setting('aimee.p7_rewrap_fence')::bigint);
-- Owner/migration-side corruption cannot bypass finish: prove both an extra row
-- and a substituted logical identity are rejected, then restore the exact set.
INSERT INTO kb_vault_rewrap_dek_stage(operation_id,source_id,principal,agent,cred,version,
  source_digest,new_wrapped_dek) VALUES('cccccccccccccccccccccccccccccccc',9223372036854775807,
  'extra','x','y',1,decode(repeat('01',32),'hex'),decode(repeat('02',40),'hex'));
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_stage_finish(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'40001');
DELETE FROM kb_vault_rewrap_dek_stage WHERE operation_id='cccccccccccccccccccccccccccccccc'
  AND source_id=9223372036854775807;
DO $$ DECLARE sid BIGINT; old_agent TEXT;
BEGIN
  SELECT source_id,agent INTO STRICT sid,old_agent FROM kb_vault_rewrap_dek_stage
    WHERE operation_id='cccccccccccccccccccccccccccccccc' ORDER BY source_id LIMIT 1;
  UPDATE kb_vault_rewrap_dek_stage SET agent=agent||'-substituted'
    WHERE operation_id='cccccccccccccccccccccccccccccccc' AND source_id=sid;
  PERFORM p7_rewrap_expect_error(format('SELECT org_vault_rewrap_stage_finish(%L,%s)',
    'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')),'40001');
  UPDATE kb_vault_rewrap_dek_stage SET agent=old_agent
    WHERE operation_id='cccccccccccccccccccccccccccccccc' AND source_id=sid;
END $$;
-- Exact stage replay succeeds; a different new wrap is rejected.
DO $$ DECLARE r RECORD; n BYTEA;
BEGIN
  SELECT * INTO STRICT r FROM org_vault_rewrap_secret_page(
    'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,0,1);
  SELECT new_wrapped_dek INTO STRICT n FROM kb_vault_rewrap_dek_stage
    WHERE operation_id='cccccccccccccccccccccccccccccccc' AND source_id=r.source_id;
  PERFORM org_vault_rewrap_stage_dek('cccccccccccccccccccccccccccccccc',
    current_setting('aimee.p7_rewrap_fence')::bigint,r.source_id,r.principal,r.agent,r.cred,
    r.version,r.source_digest,n);
  BEGIN
    PERFORM org_vault_rewrap_stage_dek('cccccccccccccccccccccccccccccccc',
      current_setting('aimee.p7_rewrap_fence')::bigint,r.source_id,r.principal,r.agent,r.cred,
      r.version,r.source_digest,decode(repeat('fe',40),'hex'));
    RAISE EXCEPTION 'P7 rewrap FAIL: conflicting stage replay accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_stage_finish(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,
  'wraps_staged');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_stage_finish(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,
  'wraps_staged');
COMMIT;

BEGIN;
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_mark_resealed(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea))$q$,'40001');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_mark_committing(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,
  'reseal_committing');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_abort(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,'too_late')$q$,
  '40001');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_mark_resealed(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  decode(repeat('00',32),'hex'))$q$,'40001');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_mark_resealed(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea))$q$,'resealed');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_mark_resealed(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea))$q$,'resealed');
DO $$ DECLARE expected CONSTANT TEXT:='f8f4a2ebfc1bb8410f24460321a4d6783987ba5e5e176c8f348a28e43888690f';
BEGIN
  IF (SELECT count(*) FROM kb_vault_rewrap_worm WHERE operation_id=
       'cccccccccccccccccccccccccccccccc' AND event_kind='resealed' AND event_id=expected)<>1 THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: deterministic resealed WORM mismatch';
  END IF;
  PERFORM org_vault_rewrap_worm_append('cccccccccccccccccccccccccccccccc',
    'resealed','resealed','');
  BEGIN
    PERFORM org_vault_rewrap_worm_append('cccccccccccccccccccccccccccccccc',
      'resealed','resealed','different');
    RAISE EXCEPTION 'P7 rewrap FAIL: conflicting WORM replay accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN UPDATE kb_vault_rewrap_worm SET detail='tamper' WHERE event_kind='resealed';
    RAISE EXCEPTION 'P7 rewrap FAIL: WORM update accepted';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM LIKE 'P7 rewrap FAIL:%' THEN RAISE; END IF;
  END;
  BEGIN DELETE FROM kb_vault_rewrap_worm WHERE event_kind='resealed';
    RAISE EXCEPTION 'P7 rewrap FAIL: WORM delete accepted';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM LIKE 'P7 rewrap FAIL:%' THEN RAISE; END IF;
  END;
  BEGIN TRUNCATE kb_vault_rewrap_worm;
    RAISE EXCEPTION 'P7 rewrap FAIL: WORM truncate accepted';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM LIKE 'P7 rewrap FAIL:%' THEN RAISE; END IF;
  END;
END $$;
COMMIT;

-- A changed source after staging makes promotion fail atomically: no other wrap
-- or verifier may be promoted.  Restoring the exact source permits one atomic
-- promotion; replay returns promoted while the primary remains sealed.
BEGIN;
CREATE TEMP TABLE p7_changed AS SELECT id,wrapped_dek FROM org_vault_secret ORDER BY id LIMIT 1;
UPDATE org_vault_secret SET wrapped_dek=decode(repeat('ee',40),'hex')
 WHERE id=(SELECT id FROM p7_changed);
COMMIT;

BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_promote(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'40001');
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM org_vault_secret s JOIN kb_vault_rewrap_dek_stage x
      ON x.operation_id='cccccccccccccccccccccccccccccccc' AND x.source_id=s.id
      WHERE s.wrapped_dek=x.new_wrapped_dek) OR
     EXISTS(SELECT 1 FROM org_vault_salt s JOIN kb_vault_rewrap_check_stage x
      ON x.operation_id='cccccccccccccccccccccccccccccccc' AND x.principal=s.principal
      WHERE octet_length(s.kek_check)>0 AND s.kek_check=x.new_kek_check) THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: failed promotion partially updated inventory';
  END IF;
END $$;
UPDATE org_vault_secret s SET wrapped_dek=c.wrapped_dek FROM p7_changed c WHERE s.id=c.id;
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_promote(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'promoted');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_promote(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'promoted');
DO $$ BEGIN
  IF EXISTS(SELECT 1 FROM org_vault_secret s JOIN kb_vault_rewrap_dek_stage x
      ON x.operation_id='cccccccccccccccccccccccccccccccc' AND x.source_id=s.id
      WHERE s.wrapped_dek<>x.new_wrapped_dek) OR
     EXISTS(SELECT 1 FROM org_vault_salt s JOIN kb_vault_rewrap_check_stage x
      ON x.operation_id='cccccccccccccccccccccccccccccccc' AND x.principal=s.principal
      WHERE s.kek_check<>x.new_kek_check) OR
     NOT (SELECT sealed AND maintenance_kind='tpm2-reseal' AND
       maintenance_id='cccccccccccccccccccccccccccccccc' FROM kb_vault_control) THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: promotion result/barrier mismatch';
  END IF;
END $$;

-- Completion validates the entire promoted checkpoint, advances only the fence,
-- clears maintenance identity, retains staging, and replays from its consumed fence.
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_complete(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  decode(repeat('00',32),'hex'),
  (SELECT inventory_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'),
  (SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'))$q$,'P7I01');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_complete(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea),decode(repeat('00',32),'hex'),
  (SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'))$q$,'P7I01');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_complete(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea),
  (SELECT inventory_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'),
  decode(repeat('00',32),'hex'))$q$,'P7I01');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_complete(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint-1,
  sha256('\x72656365697074'::bytea),
  (SELECT inventory_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'),
  (SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'))$q$,'P7C01');
DO $$ DECLARE f BIGINT:=current_setting('aimee.p7_rewrap_fence')::bigint;
BEGIN
  UPDATE kb_vault_rewrap_operation SET fencing_token=9223372036854775807
    WHERE operation_id='cccccccccccccccccccccccccccccccc';
  UPDATE kb_vault_control SET fencing_token=9223372036854775807 WHERE singleton=1;
  PERFORM p7_rewrap_expect_error($q$SELECT org_vault_rewrap_complete(
    'cccccccccccccccccccccccccccccccc',9223372036854775807,
    sha256('\x72656365697074'::bytea),
    (SELECT inventory_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'),
    (SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'))$q$,'22003');
  UPDATE kb_vault_rewrap_operation SET fencing_token=f
    WHERE operation_id='cccccccccccccccccccccccccccccccc';
  UPDATE kb_vault_control SET fencing_token=f WHERE singleton=1;
END $$;
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_complete(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea),
  (SELECT inventory_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'),
  (SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'))$q$,'completed');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_complete(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea),
  (SELECT inventory_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'),
  (SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'))$q$,'completed');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_complete(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea),decode(repeat('ff',32),'hex'),
  (SELECT stage_digest FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc'))$q$,'23505');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_recovery_required(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  'after_complete')$q$,'40001');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_abort(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  'after_complete')$q$,'40001');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_record_prepared(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  '\x72656365697074',sha256('\x72656365697074'::bytea))$q$,'completed');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_stage_finish(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'completed');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_mark_committing(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'completed');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_mark_resealed(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint,
  sha256('\x72656365697074'::bytea))$q$,'completed');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_promote(
  'cccccccccccccccccccccccccccccccc',current_setting('aimee.p7_rewrap_fence')::bigint)$q$,'completed');
DO $$ DECLARE expected_event TEXT;
BEGIN
  expected_event:=encode(sha256(convert_to('aimee-vault-rewrap-worm-v1','UTF8') ||
    org_vault_rewrap_pack_text('cccccccccccccccccccccccccccccccc') ||
    org_vault_rewrap_pack_text('completed')),'hex');
  IF NOT (SELECT state='completed' AND failure_from_state IS NULL
      FROM kb_vault_rewrap_operation WHERE operation_id='cccccccccccccccccccccccccccccccc') OR
     NOT (SELECT sealed AND maintenance_kind='' AND maintenance_id='' AND
       fencing_token=current_setting('aimee.p7_rewrap_fence')::bigint+1
       FROM kb_vault_control WHERE singleton=1) OR
     NOT EXISTS(SELECT 1 FROM kb_vault_rewrap_dek_stage
       WHERE operation_id='cccccccccccccccccccccccccccccccc') OR
     (SELECT count(*) FROM kb_vault_rewrap_worm WHERE operation_id=
       'cccccccccccccccccccccccccccccccc' AND event_kind='completed' AND state='completed' AND
       event_id=expected_event AND fencing_token=current_setting('aimee.p7_rewrap_fence')::bigint AND
       receipt_digest=sha256('\x72656365697074'::bytea) AND detail='')<>1 THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: completion state/barrier/staging/outbox mismatch';
  END IF;
END $$;
COMMIT;

DROP FUNCTION p7_rewrap_recovery_case(TEXT,TEXT,TEXT,BIGINT);
DROP FUNCTION p7_rewrap_stage_all(TEXT,BIGINT);
DROP FUNCTION p7_rewrap_expect_error(TEXT,TEXT);
DROP FUNCTION p7_rewrap_expect(TEXT,TEXT);
\echo '== P7-reseal-d1 PostgreSQL completion/quarantine assertions PASSED =='
