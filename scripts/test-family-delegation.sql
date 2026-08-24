-- Verify the delegation family on a real server.
--
-- The most important assertion here is the cycle one. The Go test can only
-- check that a depth guard is textually present; whether the recursion actually
-- terminates is a database behaviour, so this builds a real cycle and requires
-- the query to come back.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS delegation_messages;
DROP TABLE IF EXISTS delegation_spawns;
DROP TABLE IF EXISTS delegation_checkpoint;
-- lifecycle_delegate_job references agent_jobs, so it goes first.
DROP TABLE IF EXISTS lifecycle_delegate_job;
DROP TABLE IF EXISTS agent_jobs;
DROP TABLE IF EXISTS delegate_learnings;

\i /tmp/family_schema_delegation.sql

-- --- the recursive walks terminate on a cycle -------------------------------------

-- A cycle in parent_delegation_id is not prevented on write. Without the depth
-- guard these CTEs recurse until the server gives up; with it they return. The
-- statement timeout is what turns "hangs" into a failure rather than a hung
-- suite.
SET statement_timeout = '10s';

-- First: prove the guard is what does the work. The SAME walk without it must
-- NOT return. Without this the positive case below would pass whether or not
-- the guard did anything -- which is exactly how the equivalent Go test failed
-- to catch a neutered guard.
DO $$
DECLARE unguarded_returned boolean := false;
BEGIN
  INSERT INTO delegation_spawns (delegation_id, parent_delegation_id) VALUES
    ('a', 'c'), ('b', 'a'), ('c', 'b');

  SET LOCAL statement_timeout = '2s';
  BEGIN
    PERFORM count(*) FROM (
      WITH RECURSIVE ancestors(delegation_id, parent_delegation_id, depth) AS (
          SELECT delegation_id, parent_delegation_id, 0
            FROM delegation_spawns WHERE delegation_id = 'a'
          UNION ALL
          SELECT s.delegation_id, s.parent_delegation_id, a.depth + 1
            FROM delegation_spawns s
            JOIN ancestors a ON s.delegation_id = a.parent_delegation_id
           WHERE a.parent_delegation_id <> ''
      )
      SELECT 1 FROM ancestors) t;
    unguarded_returned := true;
  EXCEPTION WHEN query_canceled THEN
    NULL;  -- expected: the unguarded walk never converges
  END;
  ASSERT NOT unguarded_returned,
    'the unguarded walk returned on a cycle -- the depth guard proves nothing';
END $$;

RESET statement_timeout;
SET statement_timeout = '10s';

DO $$
DECLARE root text; n bigint;
BEGIN

  -- climbing from 'a' must stop rather than loop
  WITH RECURSIVE ancestors(delegation_id, parent_delegation_id, depth) AS (
      SELECT delegation_id, parent_delegation_id, 0
        FROM delegation_spawns WHERE delegation_id = 'a'
      UNION ALL
      SELECT s.delegation_id, s.parent_delegation_id, a.depth + 1
        FROM delegation_spawns s
        JOIN ancestors a ON s.delegation_id = a.parent_delegation_id
       WHERE a.parent_delegation_id <> '' AND a.depth < 64
  )
  SELECT delegation_id INTO root FROM ancestors
   ORDER BY (parent_delegation_id = '') DESC, depth DESC LIMIT 1;
  ASSERT root IS NOT NULL, 'the ancestor walk returned nothing on a cycle';

  -- and descending must stop too
  WITH RECURSIVE descendants(delegation_id, depth) AS (
      SELECT delegation_id, 0 FROM delegation_spawns WHERE parent_delegation_id = 'a'
      UNION
      SELECT s.delegation_id, d.depth + 1
        FROM delegation_spawns s
        JOIN descendants d ON s.parent_delegation_id = d.delegation_id
       WHERE d.depth < 64
  )
  SELECT count(*) INTO n FROM descendants;
  ASSERT n >= 1, format('the descendant walk counted %s on a cycle', n);
END $$;

-- The same walks on an honest tree give the honest answers.
DO $$
DECLARE root text; n bigint;
BEGIN
  DELETE FROM delegation_spawns;
  INSERT INTO delegation_spawns (delegation_id, parent_delegation_id) VALUES
    ('root', ''), ('mid', 'root'), ('leaf1', 'mid'), ('leaf2', 'mid');

  WITH RECURSIVE ancestors(delegation_id, parent_delegation_id, depth) AS (
      SELECT delegation_id, parent_delegation_id, 0
        FROM delegation_spawns WHERE delegation_id = 'leaf1'
      UNION ALL
      SELECT s.delegation_id, s.parent_delegation_id, a.depth + 1
        FROM delegation_spawns s
        JOIN ancestors a ON s.delegation_id = a.parent_delegation_id
       WHERE a.parent_delegation_id <> '' AND a.depth < 64
  )
  SELECT delegation_id INTO root FROM ancestors
   ORDER BY (parent_delegation_id = '') DESC, depth DESC LIMIT 1;
  ASSERT root = 'root', format('the root of leaf1 is %s, want root', root);

  WITH RECURSIVE descendants(delegation_id, depth) AS (
      SELECT delegation_id, 0 FROM delegation_spawns WHERE parent_delegation_id = 'root'
      UNION
      SELECT s.delegation_id, d.depth + 1
        FROM delegation_spawns s
        JOIN descendants d ON s.parent_delegation_id = d.delegation_id
       WHERE d.depth < 64
  )
  SELECT count(*) INTO n FROM descendants;
  ASSERT n = 3, format('root has %s descendants, want 3', n);
END $$;

-- The recursive cancel flips the whole subtree and nothing outside it.
DO $$
DECLARE cancelled text[]; untouched text;
BEGIN
  DELETE FROM delegation_spawns;
  INSERT INTO delegation_spawns (id, delegation_id, parent_delegation_id, status) VALUES
    (1, 'root',  '',     'running'),
    (2, 'mid',   'root', 'running'),
    (3, 'leaf',  'mid',  'running'),
    (4, 'other', '',     'running');

  WITH RECURSIVE descendants(id, delegation_id, depth) AS (
      SELECT id, delegation_id, 0 FROM delegation_spawns WHERE id = 1
      UNION
      SELECT s.id, s.delegation_id, d.depth + 1
        FROM delegation_spawns s
        JOIN descendants d ON s.parent_delegation_id = d.delegation_id
       WHERE d.depth < 64
  )
  UPDATE delegation_spawns
     SET status = 'cancelled', completed_at = now(), updated_at = now()
   WHERE id IN (SELECT id FROM descendants) AND status = ANY(ARRAY['active','running']);

  SELECT array_agg(delegation_id ORDER BY delegation_id) INTO cancelled
    FROM delegation_spawns WHERE status = 'cancelled';
  ASSERT cancelled = ARRAY['leaf','mid','root'],
    format('cancelled %s, want the whole subtree', cancelled);

  SELECT status INTO untouched FROM delegation_spawns WHERE delegation_id = 'other';
  ASSERT untouched = 'running', 'a spawn outside the subtree was cancelled';
END $$;

RESET statement_timeout;

-- --- the lease race ------------------------------------------------------------------

-- RETURNING ties the claim to this statement. Two callers race; only one gets a
-- row back.
DO $$
DECLARE first_id bigint; second_id bigint;
BEGIN
  INSERT INTO agent_jobs (id, role, status) VALUES (1, 'reviewer', 'pending');

  UPDATE agent_jobs SET status = 'running', lease_owner = 'worker-1',
                        heartbeat_at = now(), updated_at = now()
   WHERE id = 1 AND status = 'pending'
  RETURNING id INTO first_id;

  UPDATE agent_jobs SET status = 'running', lease_owner = 'worker-2',
                        heartbeat_at = now(), updated_at = now()
   WHERE id = 1 AND status = 'pending'
  RETURNING id INTO second_id;

  ASSERT first_id = 1, 'the first claimer got no row back';
  ASSERT second_id IS NULL, 'two callers both claimed the same job';
  ASSERT (SELECT lease_owner FROM agent_jobs WHERE id = 1) = 'worker-1',
    'the loser overwrote the winner''s lease';
END $$;

-- --- the unassigned cancel ---------------------------------------------------------------

-- It may only touch a job nobody has picked up: pending, or running with no
-- agent named, and only once it has aged past the threshold.
DO $$
DECLARE got bigint;
BEGIN
  DELETE FROM agent_jobs;
  INSERT INTO agent_jobs (id, status, agent_name, created_at) VALUES
    (1, 'pending', '',       now() - interval '2 hours'),
    (2, 'running', '',       now() - interval '2 hours'),
    (3, 'running', 'agent-1', now() - interval '2 hours'),
    (4, 'pending', '',       now());

  -- a claimed running job is untouchable
  UPDATE agent_jobs SET status = 'cancelled', cancelled_at = now(), cancel_reason = 'x'
   WHERE id = 3 AND (status = 'pending' OR (status = 'running' AND btrim(agent_name) = ''))
     AND coalesce(heartbeat_at, created_at) <= now() - make_interval(secs => 3600)
  RETURNING id INTO got;
  ASSERT got IS NULL, 'a job with a named agent was cancelled';

  -- a job younger than the threshold is untouchable
  UPDATE agent_jobs SET status = 'cancelled', cancelled_at = now(), cancel_reason = 'x'
   WHERE id = 4 AND (status = 'pending' OR (status = 'running' AND btrim(agent_name) = ''))
     AND coalesce(heartbeat_at, created_at) <= now() - make_interval(secs => 3600)
  RETURNING id INTO got;
  ASSERT got IS NULL, 'a job younger than the threshold was cancelled';

  -- an aged unclaimed one is fair game
  UPDATE agent_jobs SET status = 'cancelled', cancelled_at = now(), cancel_reason = 'x'
   WHERE id = 2 AND (status = 'pending' OR (status = 'running' AND btrim(agent_name) = ''))
     AND coalesce(heartbeat_at, created_at) <= now() - make_interval(secs => 3600)
  RETURNING id INTO got;
  ASSERT got = 2, 'an aged unclaimed job was not cancelled';

  -- a heartbeat resets the age even when created_at is old
  UPDATE agent_jobs SET heartbeat_at = now() WHERE id = 1;
  UPDATE agent_jobs SET status = 'cancelled', cancelled_at = now(), cancel_reason = 'x'
   WHERE id = 1 AND (status = 'pending' OR (status = 'running' AND btrim(agent_name) = ''))
     AND coalesce(heartbeat_at, created_at) <= now() - make_interval(secs => 3600)
  RETURNING id INTO got;
  ASSERT got IS NULL, 'a recently-beating job was cancelled on its creation age';
END $$;

-- --- heartbeat staleness ------------------------------------------------------------------

DO $$
DECLARE stale boolean;
BEGIN
  SELECT (now() - interval '2 hours')::timestamptz + make_interval(mins => 30) < now() INTO stale;
  ASSERT stale, 'a two-hour-old heartbeat was not stale against a 30-minute window';

  SELECT (now() - interval '1 minute')::timestamptz + make_interval(mins => 30) < now() INTO stale;
  ASSERT NOT stale, 'a one-minute-old heartbeat was stale against a 30-minute window';
END $$;

-- --- the participant token is unique where it is set -----------------------------------------

DO $$
BEGIN
  DELETE FROM agent_jobs;
  INSERT INTO agent_jobs (participant_token) VALUES ('tok-1');
  BEGIN
    INSERT INTO agent_jobs (participant_token) VALUES ('tok-1');
    ASSERT false, 'two jobs shared a participant token';
  EXCEPTION WHEN unique_violation THEN NULL; END;

  -- but the many jobs with no token coexist, which is what the partial index is for
  INSERT INTO agent_jobs (participant_token) VALUES ('');
  INSERT INTO agent_jobs (participant_token) VALUES ('');
  ASSERT (SELECT count(*) FROM agent_jobs WHERE participant_token = '') = 2,
    'the partial index rejected untokened jobs';
END $$;

-- --- the reservation adoption -----------------------------------------------------------------

-- Adoption only happens when the prefix matches EXACTLY ONE usable seat.
--
-- The seats below name real jobs because job_id carries a reference now. A
-- seat that named an invented job used to be representable, and the terminal
-- sweep would have skipped it forever.
DO $$
DECLARE got bigint;
BEGIN
  INSERT INTO agent_jobs (id, role, status) VALUES
    (5, 'delegate', 'running'),
    (6, 'delegate', 'running');

  INSERT INTO lifecycle_delegate_job (execution_key, job_id, work_item_id) VALUES
    ('repo:stage:aaa', 5, 'work-1');

  UPDATE lifecycle_delegate_job SET execution_key = 'repo:stage:new', updated_at = now()
   WHERE execution_key = (SELECT execution_key FROM lifecycle_delegate_job
                           WHERE work_item_id = 'work-1'
                             AND left(execution_key, 11) = 'repo:stage:' AND job_id IS NOT NULL LIMIT 1)
     AND 1 = (SELECT count(*) FROM lifecycle_delegate_job
               WHERE work_item_id = 'work-1'
                 AND left(execution_key, 11) = 'repo:stage:' AND job_id IS NOT NULL)
     AND NOT EXISTS (SELECT 1 FROM lifecycle_delegate_job WHERE execution_key = 'repo:stage:new')
  RETURNING job_id INTO got;
  ASSERT got = 5, format('a sole seat was not adopted (got %s)', got);

  -- with two seats the cardinality check refuses
  DELETE FROM lifecycle_delegate_job;
  INSERT INTO lifecycle_delegate_job (execution_key, job_id, work_item_id) VALUES
    ('repo:stage:aaa', 5, 'work-1'),
    ('repo:stage:bbb', 6, 'work-1');

  UPDATE lifecycle_delegate_job SET execution_key = 'repo:stage:new', updated_at = now()
   WHERE execution_key = (SELECT execution_key FROM lifecycle_delegate_job
                           WHERE work_item_id = 'work-1'
                             AND left(execution_key, 11) = 'repo:stage:' AND job_id IS NOT NULL LIMIT 1)
     AND 1 = (SELECT count(*) FROM lifecycle_delegate_job
               WHERE work_item_id = 'work-1'
                 AND left(execution_key, 11) = 'repo:stage:' AND job_id IS NOT NULL)
     AND NOT EXISTS (SELECT 1 FROM lifecycle_delegate_job WHERE execution_key = 'repo:stage:new')
  RETURNING job_id INTO got;
  ASSERT got IS NULL, 'a grouped seat was adopted by guessing';
END $$;

-- An empty seat is NULL in the column and 0 on the wire.
--
-- The two spellings are not the same thing, and the difference is what lets
-- job_id carry a reference at all: a foreign key holds for every non-null
-- value, and no agent job has id 0, so storing the sentinel literally would
-- reject exactly the empty seats it exists to represent.
DO $$
DECLARE stored bigint; on_wire bigint; usable bigint;
BEGIN
  DELETE FROM lifecycle_delegate_job;

  INSERT INTO lifecycle_delegate_job (execution_key, job_id, work_item_id)
       VALUES ('repo:stage:empty', NULLIF(0::bigint, 0), 'work-2');

  SELECT job_id, COALESCE(job_id, 0) INTO stored, on_wire
    FROM lifecycle_delegate_job WHERE execution_key = 'repo:stage:empty';

  ASSERT stored IS NULL, 'the wire sentinel was stored literally instead of as a null';
  ASSERT on_wire = 0, format('an empty seat renders as %s on the wire, want 0', on_wire);

  -- and an empty seat is not adoptable
  SELECT count(*) INTO usable
    FROM lifecycle_delegate_job WHERE work_item_id = 'work-2' AND job_id IS NOT NULL;
  ASSERT usable = 0, 'an empty seat counted as usable';
END $$;

-- Forgetting an empty seat works. Equality is never true of a null, so a
-- caller sending 0 for "the empty one" would otherwise silently fail to clear
-- exactly the seats that most need clearing.
DO $$
DECLARE left_behind bigint;
BEGIN
  DELETE FROM lifecycle_delegate_job
   WHERE execution_key = 'repo:stage:empty'
     AND job_id IS NOT DISTINCT FROM NULLIF(0::bigint, 0);

  SELECT count(*) INTO left_behind
    FROM lifecycle_delegate_job WHERE execution_key = 'repo:stage:empty';
  ASSERT left_behind = 0, 'an empty seat could not be forgotten';
END $$;

-- Deleting the job empties its seat rather than leaving it naming a job that
-- is gone. A seat pointing at a deleted job is one the terminal sweep's join
-- skips forever: it can never be cancelled and never be reused.
DO $$
DECLARE dangling bigint; emptied bigint;
BEGIN
  DELETE FROM lifecycle_delegate_job;
  INSERT INTO lifecycle_delegate_job (execution_key, job_id, work_item_id)
       VALUES ('repo:stage:live', 5, 'work-3');

  DELETE FROM agent_jobs WHERE id = 5;

  SELECT count(*) INTO dangling
    FROM lifecycle_delegate_job s
   WHERE s.job_id IS NOT NULL
     AND NOT EXISTS (SELECT 1 FROM agent_jobs j WHERE j.id = s.job_id);
  ASSERT dangling = 0, format('%s seats name a job that no longer exists', dangling);

  SELECT count(*) INTO emptied
    FROM lifecycle_delegate_job WHERE execution_key = 'repo:stage:live' AND job_id IS NULL;
  ASSERT emptied = 1, 'the seat was not returned to empty when its job went away';
END $$;

-- And a seat cannot be created naming a job that was never launched.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO lifecycle_delegate_job (execution_key, job_id, work_item_id)
         VALUES ('repo:stage:ghost', 424242, 'work-4');
  EXCEPTION WHEN foreign_key_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a seat was created naming a job that does not exist';
END $$;

-- The presence check is a catalog lookup now, not a sqlite_master read.
DO $$
BEGIN
  ASSERT (SELECT to_regclass('lifecycle_delegate_job') IS NOT NULL),
    'the reservation table reads as absent';
  ASSERT (SELECT to_regclass('no_such_table_at_all') IS NULL),
    'to_regclass reports a table that does not exist';
END $$;

-- --- learning eviction ---------------------------------------------------------------------------

-- Acted-on entries go before pending ones: nobody has looked at a pending one
-- yet, so dropping it loses information a reviewer has not seen.
DO $$
DECLARE remaining text[];
BEGIN
  INSERT INTO delegate_learnings (lesson, review_status, created_at) VALUES
    ('oldest-reviewed', 'reviewed', now() - interval '3 days'),
    ('older-rejected',  'rejected', now() - interval '2 days'),
    ('old-pending',     'pending',  now() - interval '1 day'),
    ('new-pending',     'pending',  now());

  DELETE FROM delegate_learnings WHERE id IN (
      SELECT id FROM delegate_learnings
       WHERE review_status = ANY(ARRAY['reviewed','rejected'])
       ORDER BY created_at ASC, id ASC LIMIT 1);

  SELECT array_agg(lesson ORDER BY lesson) INTO remaining FROM delegate_learnings;
  ASSERT remaining = ARRAY['new-pending','old-pending','older-rejected'],
    format('after eviction: %s', remaining);
END $$;

DO $$
BEGIN
  BEGIN
    INSERT INTO delegate_learnings (lesson, confidence) VALUES ('x', 1.5);
    ASSERT false, 'a confidence above 1 was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO delegate_learnings (lesson, review_status) VALUES ('x', 'maybe');
    ASSERT false, 'an unrecognised review status was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

DROP TABLE delegation_messages;
DROP TABLE delegation_spawns;
DROP TABLE delegation_checkpoint;
DROP TABLE lifecycle_delegate_job;
DROP TABLE agent_jobs;
DROP TABLE delegate_learnings;

\echo 'DELEGATION FAMILY SUITE PASSED'
