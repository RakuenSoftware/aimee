-- P7-witness: the witness cadence, boot check, and release gate run on the kb's
-- RUNTIME connection (aimee_kb_runtime on the hardened tier), NOT as the owner. A
-- bug where the runtime role lacked the necessary grants went undetected for a long
-- time because every other witness test ran as the owner/superuser. This test runs
-- the full cadence/boot/gate surface AS aimee_kb_runtime and asserts:
--   (1) every operation the cadence/boot/gate performs SUCCEEDS, and
--   (2) every forge / control-row path STAYS DENIED (least privilege).
--
-- Requires the three-role split + schema_grants applied (so aimee_kb_runtime exists
-- with the E2 grants). Provisioned by run-p1-rls-gate.sh.
\set ON_ERROR_STOP on

-- Setup AS OWNER: a shard with a couple of records to checkpoint over.
DO $$
BEGIN
  PERFORM org_vault_witness_append(1::smallint,'rt-seed-1','!kb','!reseal','','p','','g',
    '2026-07-23T00:00:00Z',decode(repeat('b1',32),'hex'),false,decode(repeat('00',32),'hex'));
  PERFORM org_vault_witness_append(1::smallint,'rt-seed-2','!kb','!reseal','','p','','g',
    '2026-07-23T00:00:01Z',decode(repeat('b2',32),'hex'),false,decode(repeat('00',32),'hex'));
  RAISE NOTICE 'RUNTIME setup: seeded 2 records as owner';
END $$;

SET ROLE aimee_kb_runtime;
DO $$ BEGIN
  IF current_user <> 'aimee_kb_runtime' THEN
    RAISE EXCEPTION 'RUNTIME FAIL: not acting as aimee_kb_runtime (%))', current_user;
  END IF;
END $$;

-- ============================ POSITIVE: must SUCCEED ========================
-- The exact operations db2_witness_checkpoint_produce / _emit / boot / gate perform.
DO $$
DECLARE v_fence BIGINT; v_leaves BIGINT; v_max BIGINT; v_ns BIGINT; v_persisted BIGINT;
        v_cov BIGINT; v_age BIGINT; v_cnt BIGINT; v_pending BIGINT; v_ts TEXT;
BEGIN
  -- producer step 1: the control fence, via the definer accessor (NOT kb_vault_control)
  v_fence := public.org_vault_witness_control_fence();
  IF v_fence IS NULL THEN RAISE EXCEPTION 'RUNTIME FAIL: control_fence returned NULL'; END IF;

  -- producer step 2: the verified leaf scan
  SELECT count(*) INTO v_leaves FROM public.org_vault_witness_checkpoint_leaves();

  -- producer steps 3-4: read the checkpoint table (previous_digest inputs, next seq)
  SELECT COALESCE(max(seq),0) INTO v_max FROM public.kb_vault_witness_checkpoint;
  v_ns := v_max + 1;
  PERFORM root,has_predecessor,signer_key_id FROM public.kb_vault_witness_checkpoint
    ORDER BY seq DESC LIMIT 1;

  -- producer step 5: timestamp helper
  v_ts := public.pg_now_text();

  -- producer step 6: persist a checkpoint end to end (definer; runs as owner, but the
  -- runtime role must be able to CALL it). A dummy-but-well-formed signature: persist
  -- stores it, it does not verify it.
  v_persisted := public.org_vault_witness_checkpoint_persist(
    v_ns, decode(repeat('11',32),'hex'), (v_ns > 1),
    CASE WHEN v_ns > 1 THEN decode(repeat('22',32),'hex') ELSE decode(repeat('00',32),'hex') END,
    1, ''::bytea, decode(repeat('33',32),'hex'), decode(repeat('44',16),'hex'),
    1::smallint, 1, decode(repeat('55',64),'hex'), v_fence);
  IF v_persisted <> v_ns THEN
    RAISE EXCEPTION 'RUNTIME FAIL: persist returned % expected %', v_persisted, v_ns;
  END IF;

  -- boot check + gate: anchor coverage, freshness, verify-window reads
  SELECT count(*) INTO v_cov FROM public.kb_vault_witness_checkpoint
    WHERE signer_key_id <> decode(repeat('44',16),'hex');
  SELECT count(*),
         COALESCE(EXTRACT(EPOCH FROM (CURRENT_TIMESTAMP - MAX(created_at)::timestamp))::bigint,0)
    INTO v_cnt, v_age FROM public.kb_vault_witness_checkpoint;

  -- emit path: pending + advance (batch/checkpoints exercised via pending's head)
  SELECT count(*) INTO v_pending FROM public.org_vault_witness_emit_pending();
  PERFORM public.org_vault_witness_emit_batch('!kb','!reseal',0,10);
  PERFORM public.org_vault_witness_emit_checkpoints(0,10);
  PERFORM public.org_vault_witness_emit_advance(1::smallint,'','', v_persisted);

  RAISE NOTICE 'RUNTIME OK: full cadence/boot/gate surface executes as runtime '
    '(fence=%, leaves=%, persisted seq=%, coverage=%, cp_count=%, pending rows=%)',
    v_fence, v_leaves, v_persisted, v_cov, v_cnt, v_pending;
END $$;

-- ============================ NEGATIVE: must STAY DENIED ====================
-- Least privilege: the runtime role must not be able to forge evidence, mutate the
-- chain, write the cursor directly, read the control row, or call the internal
-- helpers. Each must raise insufficient_privilege (42501).
DO $$
DECLARE v_ok BOOLEAN;
BEGIN
  -- forge: a direct INSERT of a witness log row
  v_ok := false;
  BEGIN
    INSERT INTO public.kb_vault_witness_log(tenant,provider,shard_seq,source_kind,source_id,
      source_hash,has_source_pred,source_pred_hash,witness_pred_hash,record_hash,event_ts,
      seal_epoch,fencing_token)
    VALUES('!kb','!reseal',999,1,'forged',decode(repeat('ee',32),'hex'),false,
      decode(repeat('00',32),'hex'),decode(repeat('00',32),'hex'),decode(repeat('ef',32),'hex'),
      '2026-07-23T00:00:00Z',1,1);
    v_ok := true;
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  IF v_ok THEN RAISE EXCEPTION 'RUNTIME FAIL: runtime could INSERT (forge) a witness log row'; END IF;

  -- forge: a direct INSERT of a checkpoint
  v_ok := false;
  BEGIN
    INSERT INTO public.kb_vault_witness_checkpoint(seq,root,has_predecessor,predecessor_digest,
      shard_count,leaf_snapshot,leaf_snapshot_digest,signer_key_id,sig_alg,sig_version,signature)
    VALUES(9999,decode(repeat('11',32),'hex'),false,decode(repeat('00',32),'hex'),1,''::bytea,
      decode(repeat('33',32),'hex'),decode(repeat('44',16),'hex'),1::smallint,1,decode(repeat('55',64),'hex'));
    v_ok := true;
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  IF v_ok THEN RAISE EXCEPTION 'RUNTIME FAIL: runtime could INSERT a forged checkpoint'; END IF;

  -- mutate the chain: UPDATE a checkpoint (no UPDATE grant; WORM would also block)
  v_ok := false;
  BEGIN
    UPDATE public.kb_vault_witness_checkpoint SET root = decode(repeat('00',32),'hex');
    v_ok := true;
  EXCEPTION WHEN insufficient_privilege OR raise_exception THEN NULL; END;
  IF v_ok THEN RAISE EXCEPTION 'RUNTIME FAIL: runtime could UPDATE a checkpoint'; END IF;

  -- write the cursor directly (must go through the definer advance only)
  v_ok := false;
  BEGIN
    UPDATE public.kb_vault_witness_emit_cursor SET last_emitted = 0;
    v_ok := true;
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  IF v_ok THEN RAISE EXCEPTION 'RUNTIME FAIL: runtime could UPDATE the emit cursor directly'; END IF;

  -- read the control row directly (owner-only; only the fence definer is allowed)
  v_ok := false;
  BEGIN
    PERFORM fencing_token FROM public.kb_vault_control WHERE singleton=1;
    v_ok := true;
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  IF v_ok THEN RAISE EXCEPTION 'RUNTIME FAIL: runtime could read kb_vault_control directly'; END IF;

  -- call the internal append directly (must be reachable only nested in a definer)
  v_ok := false;
  BEGIN
    PERFORM public.org_vault_witness_append(1::smallint,'x','!kb','!reseal','','p','','g',
      '2026-07-23T00:00:00Z',decode(repeat('b3',32),'hex'),false,decode(repeat('00',32),'hex'));
    v_ok := true;
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  IF v_ok THEN RAISE EXCEPTION 'RUNTIME FAIL: runtime could call org_vault_witness_append directly'; END IF;

  -- call verify_shard directly (reachable only nested in the leaf scan)
  v_ok := false;
  BEGIN
    PERFORM public.org_vault_witness_verify_shard('!kb','!reseal');
    v_ok := true;
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  IF v_ok THEN RAISE EXCEPTION 'RUNTIME FAIL: runtime could call org_vault_witness_verify_shard directly'; END IF;

  RAISE NOTICE 'RUNTIME OK: forge / control-row / internal-helper paths all denied';
END $$;

-- ============================ HARDENED PRE-PROVISIONED VERIFY ================
-- On the hardened tier the kb connects as this runtime role and CANNOT apply DDL;
-- db2_init runs db2_verify_pre_provisioned() instead — a read-only check that the
-- schema was migrated (dim + version recorded) and its objects are present. Exercise
-- exactly those read-only queries AS the runtime role so a regression that made any
-- of them unreadable (or the metadata unrecorded) is caught here.
DO $$
DECLARE v_dim TEXT; v_ver TEXT; v_ok BOOLEAN;
BEGIN
  SELECT value INTO v_dim FROM public.kb_meta WHERE key='schema_embedding_dim';
  SELECT value INTO v_ver FROM public.kb_meta WHERE key='schema_version';
  IF v_dim IS NULL OR v_ver IS NULL THEN
    RAISE EXCEPTION 'RUNTIME FAIL: schema build metadata not recorded (dim=% ver=%) — a hardened '
      'runtime kb could not verify the migration', v_dim, v_ver;
  END IF;
  IF (public.to_regclass('public.kb_documents') IS NULL)
     OR (public.to_regclass('public.kb_vault_witness_checkpoint') IS NULL)
     OR (public.to_regprocedure('public.org_vault_witness_control_fence()') IS NULL) THEN
    RAISE EXCEPTION 'RUNTIME FAIL: a required schema object is not visible to the runtime role';
  END IF;
  RAISE NOTICE 'RUNTIME OK: hardened pre-provisioned verify inputs readable as runtime '
    '(dim=%, version=%, required objects present)', v_dim, v_ver;
END $$;

RESET ROLE;

-- ============================ CATALOG: the grant model itself ================
-- Assert the exact privileges structurally, so a REMOVED grant (or an ADDED write
-- grant) is caught even without running the cadence — the failure mode that let the
-- original bug ship. Uses has_*_privilege as the owner (RESET ROLE above).
DO $$
DECLARE
  r TEXT := 'aimee_kb_runtime';
  t TEXT;
  fn TEXT;
  needed_exec TEXT[] := ARRAY[
    'org_vault_witness_control_fence()',
    'org_vault_witness_checkpoint_leaves()',
    'org_vault_witness_checkpoint_persist(bigint,bytea,boolean,bytea,bigint,bytea,bytea,bytea,smallint,integer,bytea,bigint)',
    'org_vault_witness_emit_pending()',
    'org_vault_witness_emit_batch(text,text,bigint,integer)',
    'org_vault_witness_emit_checkpoints(bigint,integer)',
    'org_vault_witness_emit_advance(smallint,text,text,bigint)'];
  denied_exec TEXT[] := ARRAY[
    'org_vault_witness_append(smallint,text,text,text,text,text,text,text,text,bytea,boolean,bytea)',
    'org_vault_witness_verify_shard(text,text)'];
BEGIN
  -- Read-only on the four evidence tables: SELECT yes, write no.
  FOREACH t IN ARRAY ARRAY['kb_vault_witness_shard','kb_vault_witness_log',
                           'kb_vault_witness_checkpoint','kb_vault_witness_emit_cursor'] LOOP
    IF NOT has_table_privilege(r, 'public.'||t, 'SELECT') THEN
      RAISE EXCEPTION 'RUNTIME FAIL: % lacks SELECT on %', r, t;
    END IF;
    IF has_table_privilege(r, 'public.'||t, 'INSERT')
       OR has_table_privilege(r, 'public.'||t, 'UPDATE')
       OR has_table_privilege(r, 'public.'||t, 'DELETE') THEN
      RAISE EXCEPTION 'RUNTIME FAIL: % has a WRITE privilege on % (forge risk)', r, t;
    END IF;
  END LOOP;
  -- Cadence/boot/gate functions: EXECUTE granted.
  FOREACH fn IN ARRAY needed_exec LOOP
    IF NOT has_function_privilege(r, 'public.'||fn, 'EXECUTE') THEN
      RAISE EXCEPTION 'RUNTIME FAIL: % lacks EXECUTE on %', r, fn;
    END IF;
  END LOOP;
  -- Internal helpers + the control row: NOT reachable by runtime.
  FOREACH fn IN ARRAY denied_exec LOOP
    IF has_function_privilege(r, 'public.'||fn, 'EXECUTE') THEN
      RAISE EXCEPTION 'RUNTIME FAIL: % should NOT have EXECUTE on %', r, fn;
    END IF;
  END LOOP;
  IF has_table_privilege(r, 'public.kb_vault_control', 'SELECT') THEN
    RAISE EXCEPTION 'RUNTIME FAIL: % can read kb_vault_control (must be owner-only)', r;
  END IF;
  RAISE NOTICE 'RUNTIME OK: grant catalog is exactly least-privilege (read evidence, execute '
    'cadence funcs; no write, no control row, no internal helpers)';
END $$;

DO $$ BEGIN RAISE NOTICE 'p7_witness_runtime_role_pg_test: all checks passed'; END $$;
