-- Transactional source-plus-witness atomicity for the independent vault-open
-- witness ledger. SQLite WORM recovery is tested separately because it crosses
-- a PostgreSQL/SQLite transaction boundary and intentionally does not depend on
-- this witness path.
\set ON_ERROR_STOP on

-- Rollback leaves neither the source event nor its witness row.
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
    RAISE EXCEPTION 'simulated kill after source+witness write';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM <> 'simulated kill after source+witness write' THEN RAISE; END IF;
  END;

  IF (SELECT count(*) FROM public.kb_vault_open_event) <> v_before THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: open event survived a rolled-back transaction';
  END IF;
  IF (SELECT COALESCE(max(seq),0) FROM public.kb_vault_witness_shard
        WHERE tenant='!kb' AND provider='!open') <> v_head_before THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: witness head advanced without a source event';
  END IF;
  RAISE NOTICE 'ATOMICITY OK: open rollback leaves neither source nor witness';
END $$;

-- Commit leaves both, the head matches, and sequences are gap-free.
DO $$
DECLARE v_eid TEXT; v_hash BYTEA; v_seq BIGINT; v_head BYTEA; v_n BIGINT;
BEGIN
  SELECT event_id, row_hash INTO v_eid, v_hash
    FROM aimee_kb_vault_orchestrator_api.org_vault_open_idle(
      'uid:0', 'bbbbccccddddeeee0000111122223333', 5, 5, 0);

  SELECT shard_seq, record_hash INTO v_seq, v_head FROM public.kb_vault_witness_log
    WHERE tenant='!kb' AND provider='!open' AND source_id=v_eid;
  IF v_seq IS NULL THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: committed open event has no witness row';
  END IF;
  IF (SELECT head_hash FROM public.kb_vault_witness_shard
        WHERE tenant='!kb' AND provider='!open') <> v_head THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: witness head does not equal latest record hash';
  END IF;
  SELECT count(*) INTO v_n FROM public.kb_vault_witness_log
    WHERE tenant='!kb' AND provider='!open';
  IF v_n <> (SELECT max(shard_seq) FROM public.kb_vault_witness_log
               WHERE tenant='!kb' AND provider='!open') THEN
    RAISE EXCEPTION 'ATOMICITY FAIL: open witness shard has a sequence gap';
  END IF;
  PERFORM public.org_vault_witness_verify_shard('!kb','!open');
  RAISE NOTICE 'ATOMICITY OK: open commit leaves source and witness consistent';
END $$;

DO $$ BEGIN RAISE NOTICE 'p7_witness_atomicity_pg_test: all checks passed'; END $$;
