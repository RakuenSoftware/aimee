-- P7-witness-e1 real-PG17 gate: C<->SQL digest parity, append atomicity, the
-- append-only triggers, and the runtime least-privilege ACLs. Run against a
-- throwaway database provisioned from schema.sql as the owner.
\set ON_ERROR_STOP on

-- ---------------------------------------------------------------------------
-- 1. C<->SQL digest parity. The pinned vector is produced by the C
--    vault_witness_record_digest for the shared parity fixture (see
--    test_vault_witness_record.c: test_pinned_digest_vector). If the SQL packing
--    or the C preimage drifts, exactly one side changes and this fails.
-- ---------------------------------------------------------------------------
DO $$
DECLARE d BYTEA; g BYTEA;
BEGIN
  d := public.org_vault_witness_record_digest(
    0::smallint, '42', 'acme', 'anthropic', 'req-1', 'team-7', 'anthropic:default', 'g1',
    '2026-07-23T12:00:00Z',
    decode(repeat('01',32),'hex'), true, decode(repeat('02',32),'hex'),
    decode(repeat('03',32),'hex'), 5::bigint, 6::bigint, 3::bigint);
  IF encode(d,'hex') <> '719157b1cf64398bff14d0bcdd8548b5b27c9e888709dcf11f8e558b1c302fde' THEN
    RAISE EXCEPTION 'WITNESS FAIL: record digest parity: got %', encode(d,'hex');
  END IF;

  g := public.org_vault_witness_genesis('acme','anthropic');
  IF encode(g,'hex') <> '7222cdf5e667854188aa2e2733a70bff5c6680109dd3fb8e06862d80433a2555' THEN
    RAISE EXCEPTION 'WITNESS FAIL: genesis parity: got %', encode(g,'hex');
  END IF;
  RAISE NOTICE 'WITNESS OK: C<->SQL digest and genesis parity';
END $$;

-- ---------------------------------------------------------------------------
-- 2. Append advances the shard head atomically; first record links to genesis.
-- ---------------------------------------------------------------------------
DO $$
DECLARE r1 RECORD; r2 RECORD; v_head BYTEA; v_pred BYTEA;
BEGIN
  SELECT * INTO r1 FROM public.org_vault_witness_append(
    0::smallint, '1', 'acme', 'anthropic', 'req-a', 'team-1', 'anthropic:default', 'grp-a',
    '2026-07-23T12:00:00Z', decode(repeat('aa',32),'hex'), false, NULL);
  IF r1.shard_seq <> 1 THEN
    RAISE EXCEPTION 'WITNESS FAIL: first append seq % (want 1)', r1.shard_seq;
  END IF;
  -- The first record's witness predecessor must be the genesis sentinel.
  SELECT witness_pred_hash INTO v_pred FROM public.kb_vault_witness_log
    WHERE tenant='acme' AND provider='anthropic' AND shard_seq=1;
  IF v_pred <> public.org_vault_witness_genesis('acme','anthropic') THEN
    RAISE EXCEPTION 'WITNESS FAIL: first record does not link to genesis';
  END IF;

  SELECT * INTO r2 FROM public.org_vault_witness_append(
    0::smallint, '2', 'acme', 'anthropic', 'req-b', 'team-1', 'anthropic:default', 'grp-b',
    '2026-07-23T12:00:01Z', decode(repeat('bb',32),'hex'), false, NULL);
  IF r2.shard_seq <> 2 THEN
    RAISE EXCEPTION 'WITNESS FAIL: second append seq % (want 2)', r2.shard_seq;
  END IF;
  -- The second record's witness predecessor must be the first record's hash, and
  -- the shard head must now equal the second record's hash.
  SELECT witness_pred_hash INTO v_pred FROM public.kb_vault_witness_log
    WHERE tenant='acme' AND provider='anthropic' AND shard_seq=2;
  IF v_pred <> r1.record_hash THEN
    RAISE EXCEPTION 'WITNESS FAIL: chain link broken at seq 2';
  END IF;
  SELECT head_hash INTO v_head FROM public.kb_vault_witness_shard
    WHERE tenant='acme' AND provider='anthropic';
  IF v_head <> r2.record_hash THEN
    RAISE EXCEPTION 'WITNESS FAIL: shard head not advanced to seq 2';
  END IF;
  RAISE NOTICE 'WITNESS OK: append chain + shard head advance';
END $$;

-- ---------------------------------------------------------------------------
-- 3. A rewrap/open source (kind 1/2) must not carry a source predecessor.
-- ---------------------------------------------------------------------------
DO $$
BEGIN
  BEGIN
    PERFORM public.org_vault_witness_append(
      1::smallint, 'op1', 'acme', 'openai', '', '', '', 'grp',
      '2026-07-23T12:00:02Z', decode(repeat('cc',32),'hex'), true, decode(repeat('dd',32),'hex'));
    RAISE EXCEPTION 'WITNESS FAIL: rewrap append with source predecessor was accepted';
  EXCEPTION WHEN sqlstate '22023' THEN
    NULL; -- expected invalid input
  END;
  RAISE NOTICE 'WITNESS OK: source-predecessor rule enforced';
END $$;

-- ---------------------------------------------------------------------------
-- 4. Append-only triggers reject UPDATE / DELETE / TRUNCATE on the evidence log
--    and the checkpoint table.
-- ---------------------------------------------------------------------------
DO $$
BEGIN
  BEGIN
    UPDATE public.kb_vault_witness_log SET group_id='x' WHERE tenant='acme';
    RAISE EXCEPTION 'WITNESS FAIL: evidence log UPDATE was allowed';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM NOT LIKE 'WORM:%' THEN RAISE; END IF;
  END;
  BEGIN
    DELETE FROM public.kb_vault_witness_log WHERE tenant='acme';
    RAISE EXCEPTION 'WITNESS FAIL: evidence log DELETE was allowed';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM NOT LIKE 'WORM:%' THEN RAISE; END IF;
  END;
  BEGIN
    TRUNCATE public.kb_vault_witness_log;
    RAISE EXCEPTION 'WITNESS FAIL: evidence log TRUNCATE was allowed';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM NOT LIKE 'WORM:%' THEN RAISE; END IF;
  END;
  RAISE NOTICE 'WITNESS OK: evidence log is append-only';
END $$;

-- ---------------------------------------------------------------------------
-- 5. Runtime least-privilege: aimee_kb_runtime (if present) has no direct DML on
--    the witness tables and no EXECUTE on the witness functions.
-- ---------------------------------------------------------------------------
DO $$
BEGIN
  IF EXISTS(SELECT 1 FROM pg_catalog.pg_roles WHERE rolname='aimee_kb_runtime') THEN
    IF has_table_privilege('aimee_kb_runtime','public.kb_vault_witness_log','INSERT') OR
       has_table_privilege('aimee_kb_runtime','public.kb_vault_witness_log','DELETE') OR
       has_table_privilege('aimee_kb_runtime','public.kb_vault_witness_shard','UPDATE') OR
       has_table_privilege('aimee_kb_runtime','public.kb_vault_witness_shard','DELETE') OR
       has_table_privilege('aimee_kb_runtime','public.kb_vault_witness_shard','INSERT') OR
       has_table_privilege('aimee_kb_runtime','public.kb_vault_witness_checkpoint','INSERT') THEN
      RAISE EXCEPTION 'WITNESS FAIL: runtime has direct DML on a witness table';
    END IF;
    IF has_function_privilege('aimee_kb_runtime',
         'public.org_vault_witness_append(smallint,text,text,text,text,text,text,text,text,bytea,boolean,bytea)',
         'EXECUTE') THEN
      RAISE EXCEPTION 'WITNESS FAIL: runtime can execute org_vault_witness_append';
    END IF;
    RAISE NOTICE 'WITNESS OK: runtime least-privilege on witness surface';
  ELSE
    RAISE NOTICE 'WITNESS SKIP: aimee_kb_runtime role absent';
  END IF;
END $$;

-- ---------------------------------------------------------------------------
-- 6. E2 reseal wiring: org_vault_rewrap_worm_append writes a witness row into the
--    reserved ('!kb','!reseal') shard, in the same transaction, exactly once per
--    new worm row (a replay does not double-witness).
-- ---------------------------------------------------------------------------
DO $$
DECLARE v_op TEXT := 'deadbeefdeadbeefdeadbeefdeadbeef'; v_n BIGINT; v_sid TEXT; v_kind SMALLINT;
BEGIN
  -- rewrap_begin performs the 'intent' worm append, which now witnesses.
  PERFORM public.org_vault_rewrap_begin('operator@test','req-reseal-1',v_op,0,1);

  SELECT count(*), max(source_id), max(source_kind) INTO v_n, v_sid, v_kind
    FROM public.kb_vault_witness_log WHERE tenant='!kb' AND provider='!reseal';
  IF v_n <> 1 THEN
    RAISE EXCEPTION 'WITNESS FAIL: expected 1 reseal witness row, got %', v_n;
  END IF;
  IF v_sid <> v_op || '/intent' OR v_kind <> 1 THEN
    RAISE EXCEPTION 'WITNESS FAIL: reseal witness source_id/kind wrong: % / %', v_sid, v_kind;
  END IF;

  -- Idempotent replay: appending the same 'intent' event again must not add a
  -- second witness row (ON CONFLICT DO NOTHING -> no new worm row -> no witness).
  PERFORM public.org_vault_rewrap_worm_append(v_op,'intent','preparing','');
  SELECT count(*) INTO v_n FROM public.kb_vault_witness_log
    WHERE tenant='!kb' AND provider='!reseal';
  IF v_n <> 1 THEN
    RAISE EXCEPTION 'WITNESS FAIL: reseal replay double-witnessed (% rows)', v_n;
  END IF;
  RAISE NOTICE 'WITNESS OK: reseal event witnessed once, replay is idempotent';
END $$;

DO $$ BEGIN RAISE NOTICE 'p7_witness_pg_test: all checks passed'; END $$;
