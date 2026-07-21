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
    next_wrap:=sha256(int8send(r.source_id)||decode('a1','hex'))||
      substring(sha256(int8send(r.source_id)||decode('a2','hex')) FROM 1 FOR 8);
    PERFORM org_vault_rewrap_stage_dek(p_op,p_fence,r.source_id,r.principal,r.agent,r.cred,
      r.version,r.source_digest,next_wrap);
  END LOOP;
  FOR r IN SELECT * FROM org_vault_rewrap_check_page(p_op,p_fence,'',128) LOOP
    next_check:=CASE WHEN octet_length(r.kek_check)=0 THEN ''::bytea ELSE
      sha256(convert_to(r.principal,'UTF8')||decode('b1','hex'))||
      substring(sha256(convert_to(r.principal,'UTF8')||decode('b2','hex')) FROM 1 FOR 8) END;
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
  'org_vault_rewrap_mark_committing','org_vault_rewrap_mark_resealed',
  'org_vault_rewrap_pack_bytes','org_vault_rewrap_pack_text','org_vault_rewrap_promote',
  'org_vault_rewrap_record_prepared','org_vault_rewrap_recovery_required',
  'org_vault_rewrap_secret_page','org_vault_rewrap_stage_check',
  'org_vault_rewrap_stage_dek','org_vault_rewrap_stage_finish','org_vault_rewrap_status',
  'org_vault_rewrap_worm_append','org_vault_rewrap_worm_block'];
DECLARE f RECORD;
BEGIN
  IF (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
      WHERE n.nspname='public' AND c.relname=ANY(tables) AND c.relkind IN ('r','p'))<>
      cardinality(tables) OR
     NOT EXISTS(SELECT 1 FROM pg_index i JOIN pg_class x ON x.oid=i.indexrelid
       JOIN pg_namespace n ON n.oid=x.relnamespace WHERE n.nspname='public' AND
       x.relname='idx_kb_vault_rewrap_one_active' AND i.indisunique AND i.indpred IS NOT NULL) THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: table/active-index inventory drift';
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
    IF NOT f.proconfig @> ARRAY['search_path=pg_catalog, public, pg_temp']::TEXT[] OR
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

-- Post-commit failure path: recovery-required is legal only after committing,
-- is immutable/idempotent, advances the fence, and retains the sealed barrier.
BEGIN;
SELECT set_config('aimee.p7_recovery_fence',fencing_token::text,false)
  FROM org_vault_rewrap_begin('owner','request-recovery','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',11,12);
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_record_prepared(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint,
  '\x7265636f76657279',sha256('\x7265636f76657279'::bytea))$q$,'custody_prepared');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_recovery_required(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint,
  'too_early')$q$,'40001');
COMMIT;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT p7_rewrap_stage_all('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
  current_setting('aimee.p7_recovery_fence')::bigint);
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_stage_finish(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint)$q$,
  'wraps_staged');
COMMIT;
BEGIN;
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_mark_committing(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint)$q$,
  'reseal_committing');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_mark_committing(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint)$q$,
  'reseal_committing');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_recovery_required(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint,
  'mock_commit_failed')$q$,'recovery_required');
SELECT p7_rewrap_expect_error($q$SELECT org_vault_rewrap_recovery_required(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint,
  'different')$q$,'23505');
SELECT p7_rewrap_expect($q$SELECT org_vault_rewrap_recovery_required(
  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',current_setting('aimee.p7_recovery_fence')::bigint,
  'mock_commit_failed')$q$,'recovery_required');
DO $$ BEGIN
  IF NOT (SELECT sealed AND maintenance_kind='tpm2-reseal' AND
      maintenance_id='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' FROM kb_vault_control) OR
     (SELECT count(*) FROM kb_vault_rewrap_worm WHERE operation_id=
       'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb' AND event_kind='recovery_required')<>1 THEN
    RAISE EXCEPTION 'P7 rewrap FAIL: recovery-required barrier/WORM mismatch';
  END IF;
END $$;
-- Test-only owner reset permits the independent promotion scenario.  The public
-- API intentionally has no recovery clear operation in this slice.
UPDATE kb_vault_control SET sealed=false,maintenance_kind='',maintenance_id='',
  updated_at=pg_now_text() WHERE singleton=1;
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
COMMIT;

DROP FUNCTION p7_rewrap_stage_all(TEXT,BIGINT);
DROP FUNCTION p7_rewrap_expect_error(TEXT,TEXT);
DROP FUNCTION p7_rewrap_expect(TEXT,TEXT);
\echo '== P7-reseal-c PostgreSQL staging/promotion assertions PASSED =='
