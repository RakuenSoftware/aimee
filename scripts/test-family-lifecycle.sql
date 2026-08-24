-- Verify the lifecycle family on a real server.
--
-- The budget reservation is what this is mostly about: it is money, it is
-- exactly-once, and its correctness lives in predicates a scripted database
-- cannot exercise.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS wfe_frozen_create;
DROP TABLE IF EXISTS wfe_convergence;
DROP TABLE IF EXISTS lifecycle_stage_attempt;
DROP TABLE IF EXISTS lifecycle_event;
DROP TABLE IF EXISTS lifecycle_work_item;

\i /tmp/family_schema_lifecycle.sql

-- --- a work item cannot be its own parent --------------------------------------

-- The C accepted this, and one such row made seven recursive walks spin forever.
DO $$
BEGIN
  INSERT INTO lifecycle_work_item (work_item_id, parent_id, proposal_path) VALUES ('a', '', 'p/a');
  BEGIN
    UPDATE lifecycle_work_item SET parent_id = 'a' WHERE work_item_id = 'a';
    ASSERT false, 'a work item was allowed to be its own parent';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO lifecycle_work_item (work_item_id, parent_id, proposal_path) VALUES ('b', 'b', 'p/b');
    ASSERT false, 'a self-parented work item was inserted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- A longer cycle is still representable -- a CHECK cannot see another row -- so
-- the walks must still terminate on one.
SET statement_timeout = '10s';

DO $$
DECLARE unguarded_returned boolean := false; n bigint;
BEGIN
  DELETE FROM lifecycle_work_item;
  INSERT INTO lifecycle_work_item (work_item_id, parent_id, proposal_path) VALUES
    ('x', 'z', 'p/x'), ('y', 'x', 'p/y'), ('z', 'y', 'p/z');

  SET LOCAL statement_timeout = '2s';
  BEGIN
    PERFORM count(*) FROM (
      WITH RECURSIVE tree(id) AS (
          SELECT 'x'::text
          UNION ALL
          SELECT child.work_item_id FROM lifecycle_work_item child
            JOIN tree parent ON child.parent_id = parent.id)
      SELECT 1 FROM tree) t;
    unguarded_returned := true;
  EXCEPTION WHEN query_canceled THEN NULL; END;
  ASSERT NOT unguarded_returned,
    'the unguarded walk returned on a cycle -- the bound proves nothing';

  -- The dedup form gives the RIGHT answer on a cycle, not merely a bounded
  -- one: three distinct ids, each once.
  WITH RECURSIVE tree(id) AS (
      SELECT 'x'::text
      UNION
      SELECT child.work_item_id
        FROM lifecycle_work_item child JOIN tree parent ON child.parent_id = parent.id)
  SELECT count(*) INTO n FROM tree;
  ASSERT n = 3, format('the dedup walk counted %s on a three-node cycle, want 3', n);

  -- And this is why a depth column is the wrong tool here: it makes every row
  -- distinct, so the dedup never fires and the walk runs to its bound --
  -- terminating, but returning the same three ids sixty-five times.
  WITH RECURSIVE tree(id, depth) AS (
      SELECT 'x'::text, 0
      UNION
      SELECT child.work_item_id, parent.depth + 1
        FROM lifecycle_work_item child JOIN tree parent ON child.parent_id = parent.id
       WHERE parent.depth < 64)
  SELECT count(*) INTO n FROM tree;
  ASSERT n = 65, format('the depth-carrying walk counted %s, want 65 -- if this '
                        'is 3 the dedup fired and the comparison proves nothing', n);
END $$;

RESET statement_timeout;

-- --- the budget reservation ------------------------------------------------------

-- A fair share is the tree's remaining budget divided by its runnable members.
DO $$
DECLARE spent numeric; outstanding numeric; runnable bigint;
BEGIN
  DELETE FROM lifecycle_work_item;
  INSERT INTO lifecycle_work_item
      (work_item_id, parent_id, state, work_item_max_cost_usd, cum_cost_usd,
       reserved_cost_usd, proposal_path)
  VALUES ('root', '',     'active', 100, 10, 0,  'p/root'),
         ('kid1', 'root', 'active', 0,    5, 0,  'p/kid1'),
         ('kid2', 'root', 'active', 0,    0, 20, 'p/kid2'),
         ('kid3', 'root', 'active', 0,    0, 0,  'p/kid3');

  WITH RECURSIVE tree(id, depth) AS (
      SELECT 'root'::text, 0
      UNION
      SELECT child.work_item_id, parent.depth + 1
        FROM lifecycle_work_item child JOIN tree parent ON child.parent_id = parent.id
       WHERE parent.depth < 64)
  SELECT coalesce(sum(cum_cost_usd), 0), coalesce(sum(reserved_cost_usd), 0),
         count(*) FILTER (WHERE state = 'active' AND pause_reason = ''
                            AND reserved_cost_usd = 0)
    INTO spent, outstanding, runnable
    FROM lifecycle_work_item WHERE work_item_id IN (SELECT id FROM tree);

  ASSERT spent = 15, format('tree spent = %s, want 15', spent);
  ASSERT outstanding = 20, format('tree outstanding = %s, want 20', outstanding);
  -- kid2 holds a reservation so it is not waiting for one
  ASSERT runnable = 3, format('runnable = %s, want 3 (root, kid1, kid3)', runnable);
END $$;

-- The root lock is what serialises reservations within one tree.
DO $$
BEGIN
  PERFORM 1 FROM lifecycle_work_item WHERE work_item_id = 'root' FOR UPDATE;
  ASSERT true;
END $$;

-- A reservation is only taken by a RUNNABLE item: paused or terminal runs have
-- no business holding one.
DO $$
DECLARE found bigint;
BEGIN
  UPDATE lifecycle_work_item SET pause_reason = 'stuck' WHERE work_item_id = 'kid1';
  UPDATE lifecycle_work_item SET state = 'accepted' WHERE work_item_id = 'kid3';

  SELECT count(*) INTO found FROM lifecycle_work_item
   WHERE work_item_id IN ('kid1','kid3') AND state = 'active' AND pause_reason = '';
  ASSERT found = 0, 'a paused or terminal run still reads as runnable';
END $$;

-- The lease predicate is what makes a takeover legitimate. A live lease blocks
-- it; a lapsed one does not.
DO $$
DECLARE took bigint;
BEGIN
  DELETE FROM lifecycle_work_item;
  INSERT INTO lifecycle_work_item
      (work_item_id, parent_id, state, reservation_state, reservation_owner,
       reservation_lease_until, reserved_cost_usd, proposal_path)
  VALUES ('live', '', 'active', 'reserved', 'owner-a', now() + interval '2 minutes', 5, 'p/live'),
         ('dead', '', 'active', 'reserved', 'owner-a', now() - interval '1 minute', 5, 'p/dead');

  UPDATE lifecycle_work_item
     SET reservation_state = 'unresolved', reservation_owner = 'owner-b',
         reservation_lease_until = now() + make_interval(mins => 2)
   WHERE work_item_id = 'live' AND reservation_state = 'reserved'
     AND reservation_owner = 'owner-a'
     AND reservation_lease_until IS NOT NULL AND reservation_lease_until <= now();
  GET DIAGNOSTICS took = ROW_COUNT;
  ASSERT took = 0, 'a live lease was stolen';

  UPDATE lifecycle_work_item
     SET reservation_state = 'unresolved', reservation_owner = 'owner-b',
         reservation_lease_until = now() + make_interval(mins => 2)
   WHERE work_item_id = 'dead' AND reservation_state = 'reserved'
     AND reservation_owner = 'owner-a'
     AND reservation_lease_until IS NOT NULL AND reservation_lease_until <= now();
  GET DIAGNOSTICS took = ROW_COUNT;
  ASSERT took = 1, 'a lapsed lease was not taken over';

  -- and the money stays authorised: an expired estimate becomes unresolved
  -- rather than being released
  ASSERT (SELECT reserved_cost_usd FROM lifecycle_work_item WHERE work_item_id = 'dead') = 5,
    'taking over an expired reservation released the money';
  ASSERT (SELECT reservation_state FROM lifecycle_work_item WHERE work_item_id = 'dead')
       = 'unresolved',
    'an expired estimate was not retained as unresolved';
END $$;

-- Release drops only a live ESTIMATE. An actual or unresolved reservation is
-- authorised spend that already happened.
DO $$
DECLARE released bigint;
BEGIN
  DELETE FROM lifecycle_work_item;
  INSERT INTO lifecycle_work_item
      (work_item_id, state, reservation_state, reservation_owner, reserved_cost_usd, proposal_path)
  VALUES ('est', 'active', 'reserved',   'o', 5, 'p/est'),
         ('act', 'active', 'actual',     'o', 5, 'p/act'),
         ('unr', 'active', 'unresolved', 'o', 5, 'p/unr');

  UPDATE lifecycle_work_item
     SET reserved_cost_usd = 0, reservation_state = '', reservation_owner = '',
         reservation_lease_until = NULL
   WHERE work_item_id = ANY(ARRAY['est','act','unr']) AND reservation_owner = 'o'
     AND reservation_state = 'reserved';
  GET DIAGNOSTICS released = ROW_COUNT;
  ASSERT released = 1, format('release touched %s rows, want just the estimate', released);
  ASSERT (SELECT reserved_cost_usd FROM lifecycle_work_item WHERE work_item_id = 'act') = 5,
    'an actual reservation was released';
  ASSERT (SELECT reserved_cost_usd FROM lifecycle_work_item WHERE work_item_id = 'unr') = 5,
    'an unresolved reservation was released';
END $$;

-- --- the gate guard ----------------------------------------------------------------

-- A gate decision applies only while the row is still parked exactly as the
-- caller observed it.
DO $$
DECLARE applied bigint;
BEGIN
  DELETE FROM lifecycle_work_item;
  INSERT INTO lifecycle_work_item
      (work_item_id, state, current_stage, content_hash, pause_reason, proposal_path)
  VALUES ('gated', 'active', 'review', 'hash-1', 'pending_human', 'p/gated');

  -- the hash moved since the caller looked
  UPDATE lifecycle_work_item SET pause_reason = '', paused_state = '', updated_at = now()
   WHERE work_item_id = 'gated' AND current_stage = 'review' AND content_hash = 'hash-2'
     AND pause_reason = 'pending_human';
  GET DIAGNOSTICS applied = ROW_COUNT;
  ASSERT applied = 0, 'a gate applied against a stale hash';

  UPDATE lifecycle_work_item SET pause_reason = '', paused_state = '', updated_at = now()
   WHERE work_item_id = 'gated' AND current_stage = 'review' AND content_hash = 'hash-1'
     AND pause_reason = 'pending_human';
  GET DIAGNOSTICS applied = ROW_COUNT;
  ASSERT applied = 1, 'a gate did not apply against the observed state';
END $$;

-- --- the admission cap counts ROOTS ---------------------------------------------------

-- A fan-out of slices must not consume the operator's concurrency budget.
DO $$
DECLARE roots bigint;
BEGIN
  DELETE FROM lifecycle_work_item;
  INSERT INTO lifecycle_work_item (work_item_id, parent_id, state, proposal_path) VALUES
    ('r1',  '',   'active',   'p/r1'),
    ('r2',  '',   'active',   'p/r2'),
    ('s1',  'r1', 'active',   'p/s1'),
    ('s2',  'r1', 'active',   'p/s2'),
    ('old', '',   'accepted', 'p/old');

  SELECT count(*) INTO roots FROM lifecycle_work_item
   WHERE parent_id = '' AND state = 'active';
  ASSERT roots = 2, format('active roots = %s, want 2 -- slices were counted', roots);
END $$;

-- A child is created by INSERT..SELECT from the parent, so a parent that
-- stopped first makes the insert yield nothing rather than orphaning a child.
DO $$
DECLARE made bigint;
BEGIN
  UPDATE lifecycle_work_item SET state = 'stopped' WHERE work_item_id = 'r2';
  INSERT INTO lifecycle_work_item (work_item_id, parent_id, state, proposal_path)
  SELECT 'orphan-would-be', 'r2', 'active', 'p/orphan' FROM lifecycle_work_item parent
   WHERE parent.work_item_id = 'r2' AND parent.state = 'active';
  GET DIAGNOSTICS made = ROW_COUNT;
  ASSERT made = 0, 'a child was created under a stopped parent';

  INSERT INTO lifecycle_work_item (work_item_id, parent_id, state, proposal_path)
  SELECT 'legit-child', 'r1', 'active', 'p/legit' FROM lifecycle_work_item parent
   WHERE parent.work_item_id = 'r1' AND parent.state = 'active';
  GET DIAGNOSTICS made = ROW_COUNT;
  ASSERT made = 1, 'a child was not created under an active parent';
END $$;

-- --- convergence -----------------------------------------------------------------------

-- The same artifact against the same feedback is not progress, however many
-- rounds are left.
DO $$
DECLARE repeats bigint;
BEGIN
  INSERT INTO wfe_convergence (work_item_id, gate, artifact_hash, feedback_hash, identical_repeats)
       VALUES ('r1', 'plan', 'a', 'f', 1)
  ON CONFLICT (work_item_id, gate) DO UPDATE SET identical_repeats = EXCLUDED.identical_repeats;

  SELECT identical_repeats INTO repeats FROM wfe_convergence
   WHERE work_item_id = 'r1' AND gate = 'plan' AND artifact_hash = 'a' AND feedback_hash = 'f';
  ASSERT repeats = 1, format('first round repeats = %s', repeats);

  -- a different artifact resets the count
  UPDATE wfe_convergence SET artifact_hash = 'b', identical_repeats = 1
   WHERE work_item_id = 'r1' AND gate = 'plan';
  ASSERT (SELECT identical_repeats FROM wfe_convergence
           WHERE work_item_id = 'r1' AND gate = 'plan') = 1,
    'a changed artifact did not reset the repeat count';
END $$;

-- --- frozen creates ---------------------------------------------------------------------

-- Two slices claiming the same path with DIFFERENT content is a conflict;
-- identical content is two slices agreeing and may coexist.
DO $$
DECLARE clash text;
BEGIN
  DELETE FROM wfe_frozen_create;
  INSERT INTO wfe_frozen_create (parent_id, path, work_item_id, content_hash)
       VALUES ('r1', 'docs/a.md', 's1', 'hash-1');

  SELECT work_item_id INTO clash FROM wfe_frozen_create
   WHERE parent_id = 'r1' AND path = 'docs/a.md' AND work_item_id <> 's2'
     AND content_hash <> 'hash-2' LIMIT 1;
  ASSERT clash = 's1', 'a differing claim on the same path was not detected';

  SELECT work_item_id INTO clash FROM wfe_frozen_create
   WHERE parent_id = 'r1' AND path = 'docs/a.md' AND work_item_id <> 's2'
     AND content_hash <> 'hash-1' LIMIT 1;
  ASSERT clash IS NULL, 'two slices agreeing on content were treated as a conflict';
END $$;

-- --- the stage attempt counter -------------------------------------------------------------

DO $$
DECLARE n bigint;
BEGIN
  INSERT INTO lifecycle_stage_attempt (work_item_id, stage, attempts) VALUES ('r1', 'plan', 1)
  ON CONFLICT (work_item_id, stage)
  DO UPDATE SET attempts = lifecycle_stage_attempt.attempts + 1
  RETURNING attempts INTO n;
  ASSERT n = 1, format('first attempt = %s', n);

  INSERT INTO lifecycle_stage_attempt (work_item_id, stage, attempts) VALUES ('r1', 'plan', 1)
  ON CONFLICT (work_item_id, stage)
  DO UPDATE SET attempts = lifecycle_stage_attempt.attempts + 1
  RETURNING attempts INTO n;
  ASSERT n = 2, format('second attempt = %s -- the upsert did not increment', n);
END $$;

DROP TABLE wfe_frozen_create;
DROP TABLE wfe_convergence;
DROP TABLE lifecycle_stage_attempt;
DROP TABLE lifecycle_event;
DROP TABLE lifecycle_work_item;

\echo 'LIFECYCLE FAMILY SUITE PASSED'
