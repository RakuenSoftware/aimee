-- P7-witness-e3 §1 (transactional half): source-plus-witness atomicity.
--
-- The kill matrix's central required outcome is:
--
--   "The source event and its witness row are both present or both absent. Never
--    one without the other, for any of the three ledgers."
--
-- For every boundary that lies INSIDE the source transaction — witness record
-- encode, shard-head UPDATE, evidence INSERT, and the combined COMMIT (kill-matrix
-- boundaries 1-4) — a process kill is indistinguishable from a transaction abort:
-- Postgres rolls back the whole transaction either way. Aborting the transaction
-- at each point therefore tests exactly the same property, and does so
-- deterministically rather than hoping a SIGKILL lands in the right microsecond.
--
-- This does NOT discharge the process-level boundaries (checkpoint committed but
-- not yet emitted, emission cursor state across a restart, recovery on boot).
-- Those are genuinely about process lifecycle and still require the daemon harness
-- on CT260.
--
-- DESTRUCTIVE: drives real vault functions and mutates kb_vault_control. Run
-- against a freshly provisioned, isolated database only.
\set ON_ERROR_STOP on

-- ---------------------------------------------------------------------------
-- A. Rollback leaves NEITHER the source event NOR its witness row (open ledger).
--    Boundary 3/4: evidence inserted, source event written, transaction dies.
-- ---------------------------------------------------------------------------
DO $$
DECLARE v_before BIGINT; v_head_before BIGINT;
BEGIN
  UPDATE public.kb_vault_control SET sealed=true, maintenance_kind='', maintenance_id='',
    seal_epoch=5, fencing_token=5, last_opened_rewrap_fence=0 WHERE singleton=1;
  SELECT count(*) INTO v_before FROM public.kb_vault_open_event;
  SELECT COALESCE(max(seq),0) INTO v_head_before FROM public.kb_vault_witness_shard
    WHERE tenant='!kb' AND provider='!open';

  BEGIN
    PERFORM aimee_kb_vault_orchestrator_api.org_vault_open_idle(
      'uid:0', 'aaaabbbbccccdddd0000111122223333', 5, 5, 0);
    -- Everything above committed nothing yet. Force the transaction to unwind from
    -- inside, which is what a kill at this boundary leaves behind.
    RAISE EXCEPTION 'simulated kill after source+witness write';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM <> 'simulated kill after source+witness write' THEN RAISE; END IF;
  END;

  IF (SELECT count(*) FROM public.kb_vault_open_event) <> v_before THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: open event survived a rolled-back transaction';
  END IF;
  IF (SELECT COALESCE(max(seq),0) FROM public.kb_vault_witness_shard
        WHERE tenant='!kb' AND provider='!open') <> v_head_before THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: witness shard head advanced without a committed source event';
  END IF;
  RAISE NOTICE 'ATOMICITY OK: open — rollback leaves neither source event nor witness row';
END $$;

-- ---------------------------------------------------------------------------
-- B. Commit leaves BOTH, consistently: the witness row exists, the shard head
--    equals that row's hash, and there is no sequence gap.
-- ---------------------------------------------------------------------------
DO $$
DECLARE v_eid TEXT; v_hash BYTEA; v_seq BIGINT; v_head BYTEA; v_n BIGINT;
BEGIN
  SELECT event_id, row_hash INTO v_eid, v_hash
    FROM aimee_kb_vault_orchestrator_api.org_vault_open_idle(
      'uid:0', 'bbbbccccddddeeee0000111122223333', 5, 5, 0);

  SELECT shard_seq, record_hash INTO v_seq, v_head FROM public.kb_vault_witness_log
    WHERE tenant='!kb' AND provider='!open' AND source_id = v_eid;
  IF v_seq IS NULL THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: committed open event has no witness row';
  END IF;
  IF (SELECT head_hash FROM public.kb_vault_witness_shard
        WHERE tenant='!kb' AND provider='!open') <> v_head THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: shard head does not equal the latest witness record hash';
  END IF;
  -- No gap: the shard's sequences must be exactly 1..max with no holes. A hole is
  -- how a suppressed record would look, so it is checked explicitly.
  SELECT count(*) INTO v_n FROM public.kb_vault_witness_log
    WHERE tenant='!kb' AND provider='!open';
  IF v_n <> (SELECT max(shard_seq) FROM public.kb_vault_witness_log
               WHERE tenant='!kb' AND provider='!open') THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: open shard has a sequence gap (% rows, max %)',
      v_n, (SELECT max(shard_seq) FROM public.kb_vault_witness_log
              WHERE tenant='!kb' AND provider='!open');
  END IF;
  PERFORM public.org_vault_witness_verify_shard('!kb','!open');
  RAISE NOTICE 'ATOMICITY OK: open — commit leaves source event and witness row consistent';
END $$;

-- ---------------------------------------------------------------------------
-- C. Same property on the AUDIT ledger, which is the one that gates key use.
-- ---------------------------------------------------------------------------
DO $$
DECLARE v_before BIGINT; v_wbefore BIGINT; v_head_before BIGINT;
BEGIN
  SELECT count(*) INTO v_before FROM public.kb_audit_event;
  SELECT count(*) INTO v_wbefore FROM public.kb_vault_witness_log
    WHERE tenant='!kb' AND provider='!audit';
  SELECT COALESCE(max(seq),0) INTO v_head_before FROM public.kb_vault_witness_shard
    WHERE tenant='!kb' AND provider='!audit';

  BEGIN
    PERFORM public.kb_audit_worm_append('kb','uid:0','vault.key_use','anthropic:default','allow','');
    RAISE EXCEPTION 'simulated kill after audit+witness write';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM <> 'simulated kill after audit+witness write' THEN RAISE; END IF;
  END;

  IF (SELECT count(*) FROM public.kb_audit_event) <> v_before THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: audit event survived a rolled-back transaction';
  END IF;
  IF (SELECT count(*) FROM public.kb_vault_witness_log
        WHERE tenant='!kb' AND provider='!audit') <> v_wbefore THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: audit witness row survived a rolled-back transaction';
  END IF;
  IF (SELECT COALESCE(max(seq),0) FROM public.kb_vault_witness_shard
        WHERE tenant='!kb' AND provider='!audit') <> v_head_before THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: audit witness head advanced without a committed audit event';
  END IF;
  RAISE NOTICE 'ATOMICITY OK: audit — rollback leaves neither audit event nor witness row';
END $$;

-- The committed audit path must leave both, and the shard must still verify.
DO $$
DECLARE v_w BIGINT;
BEGIN
  PERFORM public.kb_audit_worm_append('kb','uid:0','vault.key_use','anthropic:s2','allow','');
  SELECT count(*) INTO v_w FROM public.kb_vault_witness_log
    WHERE tenant='!kb' AND provider='!audit';
  IF v_w < 1 THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: committed audit event produced no witness row';
  END IF;
  PERFORM public.org_vault_witness_verify_shard('!kb','!audit');
  RAISE NOTICE 'ATOMICITY OK: audit — commit leaves audit event and witness row consistent';
END $$;

-- ---------------------------------------------------------------------------
-- D. A witness append that FAILS must take its source event down with it. This is
--    the direction that matters most: a source event committing without evidence
--    is precisely the hole the umbrella exists to close. The witness log's
--    shard_seq > 0 CHECK is used as the injected failure, since it fires inside
--    the same transaction exactly as a real witness failure would.
-- ---------------------------------------------------------------------------
DO $$
DECLARE v_before BIGINT; v_failed BOOLEAN := false; v_msg TEXT := ''; v_state TEXT := '';
BEGIN
  SELECT count(*) INTO v_before FROM public.kb_audit_event;
  BEGIN
    -- Poison the shard row so the next append's head advance violates a constraint.
    UPDATE public.kb_vault_witness_shard SET seq = -1
      WHERE tenant='!kb' AND provider='!audit';
    PERFORM public.kb_audit_worm_append('kb','uid:0','vault.key_use','anthropic:s3','allow','');
  EXCEPTION WHEN OTHERS THEN
    v_failed := true;
    v_msg := SQLERRM;
    v_state := SQLSTATE;
  END;
  -- The load-bearing assertion: the append FAILED rather than committing an audit
  -- event with no evidence. If the witness call were ever unwired from this path,
  -- the poisoned shard would be irrelevant and the append would succeed here.
  IF NOT v_failed THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: poisoned witness state did not fail the append — '
      'the witness call is not on the audit path';
  END IF;
  -- And the failure must come from the witness state we poisoned, not from some
  -- unrelated error that would make this test pass for the wrong reason.
  IF v_state <> '23514' AND position('witness' in lower(v_msg)) = 0 THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: append failed for an unrelated reason (% / %)',
      v_state, v_msg;
  END IF;
  IF (SELECT count(*) FROM public.kb_audit_event) <> v_before THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: audit event committed even though its witness append failed';
  END IF;
  RAISE NOTICE 'ATOMICITY OK: a failed witness append aborts its source event (% )', v_state;
END $$;

DO $$ BEGIN RAISE NOTICE 'p7_witness_atomicity_pg_test: all checks passed'; END $$;
