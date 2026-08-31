-- P7-witness-e2 wiring gate: the atomic witness append is invoked from all three
-- source ledgers (audit, reseal, open) inside their own transactions. This drives
-- REAL vault functions and manipulates kb_vault_control, so it MUST run against a
-- freshly provisioned, isolated database (not the shared P1 RLS gate DB, whose
-- vault state other P7 tests mutate). scripts/run-p7-witness-wiring.sh provisions
-- a clean DB and runs this.
\set ON_ERROR_STOP on

-- ---------------------------------------------------------------------------
-- 6. E2 open wiring: org_vault_open_idle witnesses the sealed->open transition
--    into the reserved ('!kb','!open') shard, in the same transaction. Runs before
--    the reseal test, so no active operation exists; we stage a sealed-idle vault
--    directly (test-only superuser setup) on the control row.
-- ---------------------------------------------------------------------------
DO $$
DECLARE v_n BIGINT; v_sid TEXT; v_kind SMALLINT; v_hash BYTEA; v_eid TEXT;
  v_req TEXT := 'aaaabbbbccccddddeeeeffff00001111';
BEGIN
  UPDATE public.kb_vault_control SET sealed=true, maintenance_kind='', maintenance_id='',
    seal_epoch=5, fencing_token=5, last_opened_rewrap_fence=0 WHERE singleton=1;

  SELECT event_id, row_hash INTO v_eid, v_hash
    FROM aimee_kb_vault_orchestrator_api.org_vault_open_idle('uid:0', v_req, 5, 5, 0);

  SELECT count(*), max(source_id), max(source_kind) INTO v_n, v_sid, v_kind
    FROM public.kb_vault_witness_log WHERE tenant='!kb' AND provider='!open';
  IF v_n <> 1 THEN
    RAISE EXCEPTION 'WITNESS FAIL: expected 1 open witness row, got %', v_n;
  END IF;
  IF v_sid <> v_eid OR v_kind <> 2 THEN
    RAISE EXCEPTION 'WITNESS FAIL: open witness source_id/kind wrong: % / %', v_sid, v_kind;
  END IF;
  -- source_hash must be the open row hash (content-binding), not the event id.
  IF (SELECT source_hash FROM public.kb_vault_witness_log
        WHERE tenant='!kb' AND provider='!open' AND shard_seq=1) <> v_hash THEN
    RAISE EXCEPTION 'WITNESS FAIL: open source_hash is not the open row hash';
  END IF;
  RAISE NOTICE 'WITNESS OK: idle open witnessed once with content-binding source_hash';
END $$;

-- ---------------------------------------------------------------------------
-- 7. E2 reseal wiring: org_vault_rewrap_worm_append writes a witness row into the
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

-- The SQLite WORM and observability witnesses are deliberately independent.
-- This PostgreSQL fixture tests the vault witness ledgers only; the SQLite
-- evidence bridge has its own cross-database recovery gate.

-- ---------------------------------------------------------------------------
-- Destructive head-vs-log cross-check: build a dedicated shard, verify it intact,
-- then forge a log row + advance the head and confirm verify_shard AND the
-- leaf-set builder both refuse (P7W01). Runs in this isolated (thrown-away) DB
-- because it permanently corrupts a shard.
-- ---------------------------------------------------------------------------
DO $$
DECLARE r1 RECORD; r2 RECORD; v_head BYTEA; v_forged BYTEA;
BEGIN
  SELECT * INTO r1 FROM public.org_vault_witness_append(
    0::smallint,'1','forge-t','forge-p','','p','','g','2026-07-23T00:00:00Z',
    decode(repeat('a1',32),'hex'), false, NULL);
  SELECT * INTO r2 FROM public.org_vault_witness_append(
    0::smallint,'2','forge-t','forge-p','','p','','g','2026-07-23T00:00:01Z',
    decode(repeat('a2',32),'hex'), false, NULL);

  v_head := public.org_vault_witness_verify_shard('forge-t','forge-p');
  IF v_head IS NULL OR v_head <> r2.record_hash THEN
    RAISE EXCEPTION 'WITNESS FAIL: intact shard did not verify';
  END IF;

  -- Forge: INSERT a row whose record_hash does not match its fields, advance head.
  v_forged := decode(repeat('ee',32),'hex');
  INSERT INTO public.kb_vault_witness_log(tenant,provider,shard_seq,source_kind,source_id,
    source_hash,has_source_pred,source_pred_hash,witness_pred_hash,record_hash,event_ts,
    seal_epoch,fencing_token)
  VALUES('forge-t','forge-p',3,0,'forged',decode(repeat('11',32),'hex'),false,
    decode(repeat('00',32),'hex'),v_head,v_forged,'2026-07-23T00:00:02Z',1,1);
  UPDATE public.kb_vault_witness_shard SET seq=3, head_hash=v_forged
    WHERE tenant='forge-t' AND provider='forge-p';

  BEGIN
    PERFORM public.org_vault_witness_verify_shard('forge-t','forge-p');
    RAISE EXCEPTION 'WITNESS FAIL: forged row not caught by verify_shard';
  EXCEPTION WHEN sqlstate 'P7W01' THEN NULL;
  END;
  BEGIN
    PERFORM count(*) FROM public.org_vault_witness_checkpoint_leaves();
    RAISE EXCEPTION 'WITNESS FAIL: leaves built over a forged shard';
  EXCEPTION WHEN sqlstate 'P7W01' THEN NULL;
  END;
  RAISE NOTICE 'WITNESS OK: head-vs-log cross-check catches a forged row';
END $$;

-- ---------------------------------------------------------------------------
-- Emission cursor. The cursor is a position, not evidence, but it has one
-- property that matters for correctness: it must never rewind. A rewind would
-- re-emit history every tick, which is not a data-loss bug but is a runaway one.
DO $$
DECLARE v BIGINT;
BEGIN
  -- A synthetic shard to advance within. The cursor is now bounded by the real
  -- stream head, so a test shard must exist for the monotonicity values to be
  -- legitimate positions rather than suppression attempts.
  INSERT INTO public.kb_vault_witness_shard(tenant,provider,seq,head_hash)
  VALUES('emit-t','emit-p',20,decode(repeat('cd',32),'hex'))
  ON CONFLICT (tenant,provider) DO NOTHING;

  -- Advance, then attempt to rewind: the lower value must be ignored, not applied.
  v := public.org_vault_witness_emit_advance(0::smallint,'emit-t','emit-p',10);
  IF v <> 10 THEN RAISE EXCEPTION 'WITNESS FAIL: first advance returned %', v; END IF;
  v := public.org_vault_witness_emit_advance(0::smallint,'emit-t','emit-p',4);
  IF v <> 10 THEN RAISE EXCEPTION 'WITNESS FAIL: cursor rewound to %', v; END IF;
  v := public.org_vault_witness_emit_advance(0::smallint,'emit-t','emit-p',11);
  IF v <> 11 THEN RAISE EXCEPTION 'WITNESS FAIL: forward advance returned %', v; END IF;

  -- The checkpoint stream is a single row: a keyed checkpoint cursor is rejected
  -- rather than silently creating a second, divergent checkpoint position.
  BEGIN
    PERFORM public.org_vault_witness_emit_advance(1::smallint,'t','p',1);
    RAISE EXCEPTION 'WITNESS FAIL: keyed checkpoint cursor accepted';
  EXCEPTION WHEN sqlstate '22023' THEN NULL;
  END;
  BEGIN
    PERFORM public.org_vault_witness_emit_advance(0::smallint,'emit-t','emit-p',-1);
    RAISE EXCEPTION 'WITNESS FAIL: negative cursor accepted';
  EXCEPTION WHEN sqlstate '22023' THEN NULL;
  END;
  RAISE NOTICE 'WITNESS OK: emission cursor is monotonic and rejects invalid input';
END $$;

-- The cursor must not be usable to SUPPRESS emission. Monotonicity alone does not
-- prevent that: advancing far past the head would make the emitter skip everything
-- while the backlog gauge reported healthy. An advance past the real stream head is
-- rejected outright.
DO $$
DECLARE v_head BIGINT;
BEGIN
  SELECT seq INTO v_head FROM public.kb_vault_witness_shard
    WHERE tenant='!kb' AND provider='!open';
  IF v_head IS NULL OR v_head < 1 THEN
    RAISE EXCEPTION 'WITNESS FAIL: expected a non-empty !kb/!open shard for the suppression test';
  END IF;
  BEGIN
    PERFORM public.org_vault_witness_emit_advance(0::smallint,'!kb','!open', v_head + 1000000);
    RAISE EXCEPTION 'WITNESS FAIL: cursor advanced past the stream head (emission suppressible)';
  EXCEPTION WHEN sqlstate 'P7W06' THEN NULL;
  END;
  -- An advance exactly to the head is legitimate and must still be accepted.
  IF public.org_vault_witness_emit_advance(0::smallint,'!kb','!open', v_head) <> v_head THEN
    RAISE EXCEPTION 'WITNESS FAIL: advance to the exact head was rejected';
  END IF;
  RAISE NOTICE 'WITNESS OK: emission cursor cannot be advanced past the stream head';
END $$;

-- The batch readers must never hand back more than asked, and must start strictly
-- after the cursor: an off-by-one here silently drops or duplicates evidence.
DO $$
DECLARE v_n BIGINT; v_min BIGINT;
BEGIN
  SELECT count(*), COALESCE(min(shard_seq),0) INTO v_n, v_min
    FROM public.org_vault_witness_emit_batch('!kb','!open',0,2);
  IF v_n > 2 THEN RAISE EXCEPTION 'WITNESS FAIL: batch returned % rows for limit 2', v_n; END IF;
  IF v_n > 0 AND v_min <> 1 THEN
    RAISE EXCEPTION 'WITNESS FAIL: batch from cursor 0 started at %', v_min;
  END IF;
  IF v_n > 0 THEN
    SELECT COALESCE(min(shard_seq),0) INTO v_min
      FROM public.org_vault_witness_emit_batch('!kb','!open',1,10);
    IF v_min <> 0 AND v_min <= 1 THEN
      RAISE EXCEPTION 'WITNESS FAIL: batch after cursor 1 returned seq %', v_min;
    END IF;
  END IF;
  RAISE NOTICE 'WITNESS OK: emission batch reader is bounded and cursor-exclusive';
END $$;

-- Least privilege: the emission surface is no more reachable than the rest of the
-- witness surface. PUBLIC holds nothing on the cursor table or its functions.
DO $$
DECLARE v_bad TEXT;
BEGIN
  SELECT string_agg(p.proname, ', ') INTO v_bad
    FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace
   WHERE n.nspname='public' AND p.proname LIKE 'org_vault_witness_emit%'
     AND has_function_privilege('public', p.oid, 'EXECUTE');
  IF v_bad IS NOT NULL THEN
    RAISE EXCEPTION 'WITNESS FAIL: PUBLIC can execute emission functions: %', v_bad;
  END IF;
  IF has_table_privilege('public','public.kb_vault_witness_emit_cursor','SELECT') THEN
    RAISE EXCEPTION 'WITNESS FAIL: PUBLIC can read the emission cursor table';
  END IF;
  IF NOT (SELECT relrowsecurity FROM pg_catalog.pg_class
           WHERE oid='public.kb_vault_witness_emit_cursor'::regclass) THEN
    RAISE EXCEPTION 'WITNESS FAIL: emission cursor table has RLS disabled';
  END IF;
  RAISE NOTICE 'WITNESS OK: emission surface is least-privilege and RLS-enabled';
END $$;

DO $$ BEGIN RAISE NOTICE 'p7_witness_wiring_pg_test: all checks passed'; END $$;
