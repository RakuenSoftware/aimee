-- Verify the roundtable pipeline family on a real server.
--
-- The Go tests script the database, so what only a real server settles is here:
-- that the compare-and-swap really matches nothing when the state has moved,
-- that the hierarchy cascades, that a NULL timestamp renders as '' the way the
-- wire expects, and that the CHECKs admit exactly the flag values the module
-- writes.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS roundtable_pipeline_attempts;
DROP TABLE IF EXISTS roundtable_pipeline_gates;
DROP TABLE IF EXISTS roundtable_pipeline_passes;
DROP TABLE IF EXISTS roundtable_pipeline_runs;

\i /tmp/family_schema_roundtable.sql

-- --- the compare-and-swap ---------------------------------------------------------

-- The expected state is in the WHERE clause, so of two callers racing exactly
-- one matches. A read then a write would let both pass the read.
DO $$
DECLARE won bigint; lost bigint; s text;
BEGIN
  INSERT INTO roundtable_pipeline_runs (id, idea, state) VALUES (1, 'an idea', 'drafting');

  UPDATE roundtable_pipeline_runs SET state = 'reviewing', updated_at = now()
   WHERE id = 1 AND state = 'drafting';
  GET DIAGNOSTICS won = ROW_COUNT;

  -- the loser expected 'drafting' too, but the state has moved
  UPDATE roundtable_pipeline_runs SET state = 'abandoned', updated_at = now()
   WHERE id = 1 AND state = 'drafting';
  GET DIAGNOSTICS lost = ROW_COUNT;

  ASSERT won = 1, format('the winner changed %s rows, want 1', won);
  ASSERT lost = 0, format('the loser changed %s rows, want 0', lost);
  SELECT state INTO s FROM roundtable_pipeline_runs WHERE id = 1;
  ASSERT s = 'reviewing', format('state = %s, want the winner''s', s);
END $$;

-- --- the hierarchy cascades ---------------------------------------------------------

-- Deleting a run must take its passes, its attempts and its gates. An attempt
-- hangs off a pass, so it only survives if the cascade is two levels deep.
DO $$
DECLARE leftover bigint;
BEGIN
  INSERT INTO roundtable_pipeline_passes (id, pipeline_id, phase, mode, pass_no)
       VALUES (10, 1, 'proposal', 'review', 1);
  INSERT INTO roundtable_pipeline_attempts (id, pass_id, attempt_no, run_id)
       VALUES (100, 10, 1, 'run-1');
  INSERT INTO roundtable_pipeline_gates (id, pipeline_id, gate_no) VALUES (1000, 1, 1);

  DELETE FROM roundtable_pipeline_runs WHERE id = 1;

  SELECT
    (SELECT count(*) FROM roundtable_pipeline_passes) +
    (SELECT count(*) FROM roundtable_pipeline_attempts) +
    (SELECT count(*) FROM roundtable_pipeline_gates)
  INTO leftover;
  ASSERT leftover = 0,
    format('%s child rows survived -- the cascade is not two levels deep', leftover);
END $$;

-- --- a NULL timestamp is '' on the wire ------------------------------------------------

-- terminal_at and resolved_at are NULL until the thing they record happens. The
-- wire has always carried '' for that, so the read must coalesce.
DO $$
DECLARE rendered text;
BEGIN
  INSERT INTO roundtable_pipeline_runs (id, idea) VALUES (2, 'another');
  INSERT INTO roundtable_pipeline_passes (id, pipeline_id, phase, mode, pass_no)
       VALUES (20, 2, 'proposal', 'review', 1);
  INSERT INTO roundtable_pipeline_attempts (id, pass_id, attempt_no) VALUES (200, 20, 1);

  SELECT coalesce(to_char(terminal_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), '')
    INTO rendered FROM roundtable_pipeline_attempts WHERE id = 200;
  ASSERT rendered = '', format('an unset terminal_at rendered as %L, want an empty string', rendered);

  UPDATE roundtable_pipeline_attempts SET terminal_at = '2026-08-22 09:00:00+00' WHERE id = 200;
  SELECT coalesce(to_char(terminal_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), '')
    INTO rendered FROM roundtable_pipeline_attempts WHERE id = 200;
  ASSERT rendered = '2026-08-22 09:00:00', format('terminal_at rendered as %s', rendered);

  -- and setting it back to NULL is how "not yet" is restored
  UPDATE roundtable_pipeline_attempts SET terminal_at = NULL WHERE id = 200;
  SELECT coalesce(to_char(terminal_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS'), '')
    INTO rendered FROM roundtable_pipeline_attempts WHERE id = 200;
  ASSERT rendered = '', 'terminal_at could not be cleared';
END $$;

-- --- the flag CHECKs admit exactly what the module writes ---------------------------------

DO $$
BEGIN
  -- 0 and 1 are fine
  UPDATE roundtable_pipeline_passes SET converged = 0, envelope_valid = 1 WHERE id = 20;

  BEGIN
    UPDATE roundtable_pipeline_passes SET converged = 2 WHERE id = 20;
    ASSERT false, 'a flag column accepted 2';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    UPDATE roundtable_pipeline_passes SET envelope_valid = -1 WHERE id = 20;
    ASSERT false, 'a flag column accepted -1';
  EXCEPTION WHEN check_violation THEN NULL; END;

  -- counts may not go negative
  BEGIN
    UPDATE roundtable_pipeline_passes SET blocking_count = -1 WHERE id = 20;
    ASSERT false, 'a count column accepted -1';
  EXCEPTION WHEN check_violation THEN NULL; END;

  -- but chunk_index MAY: a negative index is the synthesis member, which is how
  -- the group aggregate tells a whole-artifact check from a chunk
  UPDATE roundtable_pipeline_passes SET chunk_index = -1 WHERE id = 20;
  ASSERT (SELECT chunk_index FROM roundtable_pipeline_passes WHERE id = 20) = -1,
    'chunk_index cannot hold the synthesis marker';
END $$;

-- --- the group aggregate reads one group -----------------------------------------------

-- The counting itself lives in the module; what the store must get right is
-- which rows belong to the group.
DO $$
DECLARE members bigint;
BEGIN
  DELETE FROM roundtable_pipeline_passes;
  INSERT INTO roundtable_pipeline_passes (pipeline_id, phase, mode, pass_no, chunk_group, chunk_index)
       VALUES (2, 'review', 'm', 1, 5,  0),
              (2, 'review', 'm', 2, 5,  1),
              (2, 'review', 'm', 3, 5, -1),  -- the synthesis member
              (2, 'review', 'm', 4, 6,  0),  -- a different group
              (2, 'impl',   'm', 5, 5,  0);  -- a different phase

  SELECT count(*) INTO members
    FROM roundtable_pipeline_passes
   WHERE pipeline_id = 2 AND phase = 'review' AND chunk_group = 5;
  ASSERT members = 3, format('the group has %s members, want 3', members);
END $$;

-- --- uniqueness ---------------------------------------------------------------------------

DO $$
BEGIN
  BEGIN
    INSERT INTO roundtable_pipeline_passes (pipeline_id, phase, mode, pass_no)
         VALUES (2, 'review', 'm', 1);
    ASSERT false, 'two passes shared a (pipeline, phase, pass_no)';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;

DO $$
BEGIN
  INSERT INTO roundtable_pipeline_passes (id, pipeline_id, phase, mode, pass_no)
       VALUES (30, 2, 'gate', 'm', 1);
  INSERT INTO roundtable_pipeline_attempts (pass_id, attempt_no) VALUES (30, 1);
  BEGIN
    INSERT INTO roundtable_pipeline_attempts (pass_id, attempt_no) VALUES (30, 1);
    ASSERT false, 'two attempts shared a (pass, attempt_no)';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;

-- --- supersede leaves exactly one current attempt -------------------------------------------

DO $$
DECLARE current_count bigint; kept bigint;
BEGIN
  DELETE FROM roundtable_pipeline_attempts;
  INSERT INTO roundtable_pipeline_attempts (id, pass_id, attempt_no, is_current)
       VALUES (301, 30, 1, 1), (302, 30, 2, 1), (303, 30, 3, 1);

  UPDATE roundtable_pipeline_attempts SET is_current = 0 WHERE pass_id = 30 AND id <> 303;

  SELECT count(*) INTO current_count
    FROM roundtable_pipeline_attempts WHERE pass_id = 30 AND is_current = 1;
  ASSERT current_count = 1, format('%s attempts are current, want 1', current_count);
  SELECT id INTO kept FROM roundtable_pipeline_attempts WHERE pass_id = 30 AND is_current = 1;
  ASSERT kept = 303, format('the current attempt is %s, want 303', kept);
END $$;

-- --- the max-number reads coalesce ------------------------------------------------------------

-- A pipeline with no passes must answer 0, because the caller adds one to get
-- the next number and nothing would give it no number at all.
DO $$
DECLARE n bigint;
BEGIN
  SELECT coalesce(max(pass_no), 0) INTO n
    FROM roundtable_pipeline_passes WHERE pipeline_id = 999 AND phase = 'review';
  ASSERT n = 0, format('an empty pipeline answered %s, want 0', n);
END $$;

-- --- gate age -----------------------------------------------------------------------------------

DO $$
DECLARE over boolean;
BEGIN
  DELETE FROM roundtable_pipeline_gates;
  INSERT INTO roundtable_pipeline_gates (pipeline_id, gate_no, created_at)
       VALUES (2, 1, now() - interval '48 hours');
  INSERT INTO roundtable_pipeline_gates (pipeline_id, gate_no, created_at)
       VALUES (2, 2, now() - interval '1 hour');

  SELECT created_at < now() - make_interval(hours => 24) INTO over
    FROM roundtable_pipeline_gates WHERE pipeline_id = 2 AND gate_no = 1
    ORDER BY id DESC LIMIT 1;
  ASSERT over, 'a 48-hour-old gate was not over a 24-hour threshold';

  SELECT created_at < now() - make_interval(hours => 24) INTO over
    FROM roundtable_pipeline_gates WHERE pipeline_id = 2 AND gate_no = 2
    ORDER BY id DESC LIMIT 1;
  ASSERT NOT over, 'a one-hour-old gate was over a 24-hour threshold';
END $$;

-- The gate read takes the NEWEST gate for a (pipeline, gate_no), so a re-opened
-- gate does not read as its predecessor.
DO $$
DECLARE verdict text;
BEGIN
  DELETE FROM roundtable_pipeline_gates;
  INSERT INTO roundtable_pipeline_gates (pipeline_id, gate_no, verdict) VALUES (2, 1, 'rejected');
  INSERT INTO roundtable_pipeline_gates (pipeline_id, gate_no, verdict) VALUES (2, 1, 'approved');
  SELECT g.verdict INTO verdict FROM roundtable_pipeline_gates g
   WHERE pipeline_id = 2 AND gate_no = 1 ORDER BY id DESC LIMIT 1;
  ASSERT verdict = 'approved', format('the gate read %s, want the newest', verdict);
END $$;

-- --- terminal states -----------------------------------------------------------------------------

DO $$
DECLARE live text[];
BEGIN
  DELETE FROM roundtable_pipeline_runs;
  INSERT INTO roundtable_pipeline_runs (idea, state, admission_class) VALUES
    ('a', 'drafting',  'active'),
    ('b', 'reviewing', 'active'),
    ('c', 'done',      'active'),
    ('d', 'failed',    'active'),
    ('e', 'abandoned', 'active'),
    ('f', 'drafting',  'background');

  SELECT array_agg(idea ORDER BY idea) INTO live
    FROM roundtable_pipeline_runs
   WHERE state <> ALL(ARRAY['done','failed','abandoned']);
  ASSERT live = ARRAY['a','b','f'], format('live runs = %s', live);

  ASSERT (SELECT count(*) FROM roundtable_pipeline_runs
           WHERE admission_class = 'active'
             AND state <> ALL(ARRAY['done','failed','abandoned'])) = 2,
    'the active count is wrong';
END $$;

DROP TABLE roundtable_pipeline_attempts;
DROP TABLE roundtable_pipeline_gates;
DROP TABLE roundtable_pipeline_passes;
DROP TABLE roundtable_pipeline_runs;

\echo 'ROUNDTABLE FAMILY SUITE PASSED'
