-- Verify the agent-work family on a real server.
--
-- What only a real server settles is here: that two delegates claiming from the
-- same job take two different tasks and never two that touch the same file,
-- that a queue poll does not block every other writer, and that deleting a job
-- takes its history with it.
--
-- The telemetry schema loads too, because two of the agent-log aggregates join
-- token_audit. Both tables live in the same store; what does NOT cross the
-- family boundary is a foreign key, because each schema file loads on its own.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS cron_job_runs;
DROP TABLE IF EXISTS cron_jobs;
DROP TABLE IF EXISTS coord_job_tasks;
DROP TABLE IF EXISTS coord_jobs;
DROP TABLE IF EXISTS memory_cognify_jobs;
DROP TABLE IF EXISTS trigger_runs;
DROP TABLE IF EXISTS agent_log;
DROP TABLE IF EXISTS token_audit;

\i /tmp/family_schema_agent_work.sql
\i /tmp/family_schema_telemetry.sql

-- --- the task claim never hands out two tasks that touch the same file --------

-- The C read every held task's file list into a fixed array of 64 and compared
-- JSON documents pairwise in C, under SQLite's whole-database write lock. Here
-- the overlap is a set operation with no cap.
DO $$
DECLARE first_id bigint; second_id bigint; third_id bigint;
BEGIN
  INSERT INTO coord_jobs (id, max_concurrent) VALUES (1, 4);
  INSERT INTO coord_job_tasks (id, job_id, files, prompt) VALUES
    (1, 1, '["a.c", "b.c"]'::jsonb, 'first'),
    (2, 1, '["b.c", "c.c"]'::jsonb, 'overlaps the first'),
    (3, 1, '["d.c"]'::jsonb,        'touches nothing else');

  UPDATE coord_job_tasks SET status = 'claimed', claimed_by = 'w1', claimed_at = now()
   WHERE id = (SELECT t.id FROM coord_job_tasks t
                WHERE t.job_id = 1 AND t.status = 'pending'
                  AND NOT EXISTS (SELECT 1 FROM coord_job_tasks held
                                   WHERE held.job_id = t.job_id
                                     AND held.status IN ('claimed','running')
                                     AND held.files ?| ARRAY(
                                         SELECT jsonb_array_elements_text(t.files)))
                ORDER BY t.id LIMIT 1)
  RETURNING id INTO first_id;

  UPDATE coord_job_tasks SET status = 'claimed', claimed_by = 'w2', claimed_at = now()
   WHERE id = (SELECT t.id FROM coord_job_tasks t
                WHERE t.job_id = 1 AND t.status = 'pending'
                  AND NOT EXISTS (SELECT 1 FROM coord_job_tasks held
                                   WHERE held.job_id = t.job_id
                                     AND held.status IN ('claimed','running')
                                     AND held.files ?| ARRAY(
                                         SELECT jsonb_array_elements_text(t.files)))
                ORDER BY t.id LIMIT 1)
  RETURNING id INTO second_id;

  ASSERT first_id = 1, format('the first claim took task %s, want 1', first_id);
  -- task 2 shares b.c with task 1, so the second claimant must skip it
  ASSERT second_id = 3,
         format('the second claim took task %s, want 3 -- task 2 shares a file', second_id);

  -- and now nothing is left that does not conflict
  UPDATE coord_job_tasks SET status = 'claimed', claimed_by = 'w3', claimed_at = now()
   WHERE id = (SELECT t.id FROM coord_job_tasks t
                WHERE t.job_id = 1 AND t.status = 'pending'
                  AND NOT EXISTS (SELECT 1 FROM coord_job_tasks held
                                   WHERE held.job_id = t.job_id
                                     AND held.status IN ('claimed','running')
                                     AND held.files ?| ARRAY(
                                         SELECT jsonb_array_elements_text(t.files)))
                ORDER BY t.id LIMIT 1)
  RETURNING id INTO third_id;
  ASSERT third_id IS NULL, format('a third claim took task %s, which conflicts', third_id);
END $$;

-- The conflict check has no cap. Sixty-five held tasks is past what the C's
-- fixed array held, so a task overlapping the sixty-fifth was handed out
-- anyway and two delegates edited the same file.
DO $$
DECLARE claimed bigint;
BEGIN
  DELETE FROM coord_job_tasks; DELETE FROM coord_jobs;
  INSERT INTO coord_jobs (id, max_concurrent) VALUES (2, 100);

  INSERT INTO coord_job_tasks (job_id, status, claimed_by, claimed_at, files)
       SELECT 2, 'running', 'holder', now(),
              jsonb_build_array('held-' || g || '.c')
         FROM generate_series(1, 65) g;

  -- a pending task that overlaps ONLY the 65th held one
  INSERT INTO coord_job_tasks (id, job_id, files, prompt)
       VALUES (999, 2, '["held-65.c"]'::jsonb, 'conflicts with the last holder');

  SELECT COUNT(*) INTO claimed
    FROM coord_job_tasks t
   WHERE t.job_id = 2 AND t.status = 'pending'
     AND NOT EXISTS (SELECT 1 FROM coord_job_tasks held
                      WHERE held.job_id = t.job_id
                        AND held.status IN ('claimed','running')
                        AND held.files ?| ARRAY(
                            SELECT jsonb_array_elements_text(t.files)));

  ASSERT claimed = 0,
         'a task overlapping the 65th held task read as claimable';
END $$;

-- A file list that is not an array cannot be stored. The C's overlap check
-- returned "no conflict" whenever either document failed to parse, so a
-- malformed task was handed out alongside anything.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO coord_job_tasks (job_id, files) VALUES (2, '{"a": 1}'::jsonb);
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a task stored a file list that is not a list';
END $$;

-- Finishing a task releases it, and only its holder may finish it.
DO $$
DECLARE wrong_owner bigint; right_owner bigint; owner text;
BEGIN
  DELETE FROM coord_job_tasks;
  INSERT INTO coord_job_tasks (id, job_id, status, claimed_by, claimed_at)
       VALUES (10, 2, 'running', 'w1', now());

  UPDATE coord_job_tasks SET status = 'done', result = 'ok',
         claimed_by = '', claimed_at = NULL
   WHERE id = 10 AND claimed_by = 'someone-else' AND status IN ('claimed','running');
  GET DIAGNOSTICS wrong_owner = ROW_COUNT;

  UPDATE coord_job_tasks SET status = 'done', result = 'ok',
         claimed_by = '', claimed_at = NULL
   WHERE id = 10 AND claimed_by = 'w1' AND status IN ('claimed','running');
  GET DIAGNOSTICS right_owner = ROW_COUNT;

  SELECT claimed_by INTO owner FROM coord_job_tasks WHERE id = 10;

  ASSERT wrong_owner = 0, 'a task was finished by someone who did not hold it';
  ASSERT right_owner = 1, 'the holder could not finish its own task';
  ASSERT owner = '', 'a finished task still names a holder';
END $$;

-- Recovering an owner's work requeues what can be retried and gives up on what
-- cannot, in one statement, reporting both.
DO $$
DECLARE requeued bigint; failed bigint;
BEGIN
  DELETE FROM coord_job_tasks;
  INSERT INTO coord_job_tasks (id, job_id, status, claimed_by, claimed_at, preempt_requeues)
       VALUES (20, 2, 'running', 'lost', now(), 0),
              (21, 2, 'running', 'lost', now(), 5),
              (22, 2, 'running', 'other', now(), 0);

  WITH recovered AS (
      UPDATE coord_job_tasks
         SET status = CASE WHEN preempt_requeues + 1 > 3 THEN 'failed' ELSE 'pending' END,
             claimed_by = '', claimed_at = NULL,
             preempt_requeues = preempt_requeues + 1
       WHERE claimed_by = 'lost' AND status IN ('claimed','running')
   RETURNING status)
  SELECT COUNT(*) FILTER (WHERE status = 'pending'),
         COUNT(*) FILTER (WHERE status = 'failed')
    INTO requeued, failed FROM recovered;

  ASSERT requeued = 1, format('requeued %s, want 1', requeued);
  ASSERT failed = 1, format('failed %s, want the 1 past its retry bound', failed);
  ASSERT (SELECT claimed_by FROM coord_job_tasks WHERE id = 22) = 'other',
         'another owner''s task was recovered too';
END $$;

-- A cancelled job stays cancelled: cancellation is a decision somebody made,
-- and no count of finished tasks overrides it. The C recomputed the status
-- unconditionally, so a cancelled job whose last task finished came back done.
DO $$
DECLARE status_now text;
BEGIN
  DELETE FROM coord_job_tasks; DELETE FROM coord_jobs;
  INSERT INTO coord_jobs (id, status) VALUES (3, 'cancelled');
  INSERT INTO coord_job_tasks (job_id, status) VALUES (3, 'done');

  UPDATE coord_jobs j SET status = s.derived, updated_at = now()
    FROM (SELECT CASE
                     WHEN COUNT(*) = 0 THEN 'pending'
                     WHEN COUNT(*) FILTER (WHERE status = 'failed') > 0 THEN 'failed'
                     WHEN COUNT(*) FILTER (WHERE status <> 'done') = 0 THEN 'done'
                     WHEN COUNT(*) FILTER (WHERE status IN ('claimed','running')) > 0
                          THEN 'running'
                     ELSE 'pending' END AS derived
            FROM coord_job_tasks WHERE job_id = 3) s
   WHERE j.id = 3 AND j.status <> 'cancelled';

  SELECT status INTO status_now FROM coord_jobs WHERE id = 3;
  ASSERT status_now = 'cancelled',
         format('a cancelled job came back as %s', status_now);
END $$;

-- --- the cognify queue -----------------------------------------------------------

-- Enqueueing the same memory twice is one job.
DO $$
DECLARE jobs bigint;
BEGIN
  INSERT INTO memory_cognify_jobs (kind, memory_id) VALUES ('cognify_unit', 7)
  ON CONFLICT (kind, memory_id) DO NOTHING;
  INSERT INTO memory_cognify_jobs (kind, memory_id) VALUES ('cognify_unit', 7)
  ON CONFLICT (kind, memory_id) DO NOTHING;

  SELECT COUNT(*) INTO jobs FROM memory_cognify_jobs WHERE memory_id = 7;
  ASSERT jobs = 1, format('%s jobs queued for one memory', jobs);
END $$;

-- SKIP LOCKED is what lets two workers take two different jobs instead of one
-- waiting for the other.
CREATE EXTENSION IF NOT EXISTS dblink;

DO $$
BEGIN
  DELETE FROM memory_cognify_jobs;
  INSERT INTO memory_cognify_jobs (kind, memory_id) VALUES
    ('cognify_unit', 100), ('cognify_unit', 101);
END $$;

BEGIN;
-- this session takes the oldest
SELECT id FROM memory_cognify_jobs WHERE status = 'pending'
 ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED;

DO $$
DECLARE other_took bigint;
BEGIN
  -- a second connection must get the OTHER one, not block and not come back empty
  SELECT took INTO other_took FROM dblink('dbname=postgres',
      'SELECT id FROM memory_cognify_jobs WHERE status = ''pending''
        ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED') AS probe(took bigint);

  ASSERT other_took IS NOT NULL,
         'a second worker found nothing while one job was held: SKIP LOCKED is not in play';
  ASSERT other_took = (SELECT MAX(id) FROM memory_cognify_jobs),
         format('the second worker took %s, want the one this session is not holding', other_took);
END $$;

COMMIT;

-- --- cron jobs ---------------------------------------------------------------------

-- A job must have something to run: a script-mode job with no script fires
-- forever and does nothing.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO cron_jobs (id, schedule, mode) VALUES ('empty', '* * * * *', 'script');
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a script job with no script was accepted';

  INSERT INTO cron_jobs (id, schedule, mode, script)
       VALUES ('real', '* * * * *', 'script', 'echo hi');
END $$;

-- Editing a job is not creating one: created_at stays where it was.
CREATE TEMP TABLE cron_probe (created_at bigint);

DO $$
BEGIN
  INSERT INTO cron_probe SELECT created_at FROM cron_jobs WHERE id = 'real';
END $$;

DO $$
DECLARE was bigint; now_is bigint; sched text;
BEGIN
  SELECT created_at INTO was FROM cron_probe;

  INSERT INTO cron_jobs (id, schedule, mode, script)
       VALUES ('real', '0 * * * *', 'script', 'echo hi')
  ON CONFLICT (id) DO UPDATE SET schedule = EXCLUDED.schedule, script = EXCLUDED.script;

  SELECT created_at, schedule INTO now_is, sched FROM cron_jobs WHERE id = 'real';
  ASSERT sched = '0 * * * *', 'the edit did not land';
  ASSERT now_is = was, 'created_at moved when the schedule was edited';
END $$;

-- Deleting a job takes its run history. The C's reference had no cascade, so
-- the runs stayed behind naming a job that no longer existed.
DO $$
DECLARE leftover bigint;
BEGIN
  INSERT INTO cron_job_runs (job_id, status, output) VALUES ('real', 'ok', 'output');
  DELETE FROM cron_jobs WHERE id = 'real';
  SELECT COUNT(*) INTO leftover FROM cron_job_runs WHERE job_id = 'real';
  ASSERT leftover = 0, format('%s runs outlived their job', leftover);
END $$;

-- --- trigger runs ---------------------------------------------------------------

-- A run that finished started first.
DO $$
DECLARE rejected boolean := false;
BEGIN
  INSERT INTO trigger_runs (id, source, task) VALUES ('t1', 'github', 'do the thing');
  BEGIN
    UPDATE trigger_runs SET finished_at = now() WHERE id = 't1';
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a run finished having never started';
END $$;

-- The timestamps follow the status rather than being set separately.
DO $$
DECLARE started timestamptz; finished timestamptz;
BEGIN
  UPDATE trigger_runs
     SET status = 'running',
         started_at = CASE WHEN 'running' = 'running' THEN now() ELSE started_at END
   WHERE id = 't1';
  SELECT started_at, finished_at INTO started, finished FROM trigger_runs WHERE id = 't1';
  ASSERT started IS NOT NULL AND finished IS NULL,
         'moving to running did not stamp the start, or stamped a finish';

  UPDATE trigger_runs
     SET status = 'done',
         finished_at = CASE WHEN 'done' IN ('done','failed','cancelled')
                            THEN now() ELSE finished_at END
   WHERE id = 't1';
  SELECT started_at, finished_at INTO started, finished FROM trigger_runs WHERE id = 't1';
  ASSERT finished IS NOT NULL AND finished >= started,
         'finishing did not stamp a finish at or after the start';
END $$;

-- --- the agent log ----------------------------------------------------------------

-- The session search is DISTINCT ON, ordered by each session's most recent row.
-- The C ordered a SELECT DISTINCT by a column not in its list, which SQLite
-- resolves arbitrarily and PostgreSQL refuses outright.
DO $$
DECLARE sessions text[];
BEGIN
  INSERT INTO agent_log (role, session_id, success) VALUES
    ('reviewer', 's-old', true),
    ('reviewer', 's-old', true),
    ('reviewer', 's-new', true),
    ('planner',  's-other', true),
    ('reviewer', '',       true);

  SELECT array_agg(session_id) INTO sessions
    FROM (SELECT session_id, id
            FROM (SELECT DISTINCT ON (session_id) session_id, id
                    FROM agent_log
                   WHERE session_id <> '' AND role LIKE 'review%'
                   ORDER BY session_id, id DESC) recent
           ORDER BY id DESC) ordered;

  ASSERT sessions = ARRAY['s-new','s-old'],
         format('the search returned %s, want the two reviewer sessions newest first', sessions);
END $$;

-- confidence of -1 means "not scored", which is a different thing from 0.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO agent_log (role, confidence) VALUES ('x', -5);
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a confidence outside the scale was stored';

  INSERT INTO agent_log (role, confidence) VALUES ('unscored', -1);
  ASSERT (SELECT confidence FROM agent_log WHERE role = 'unscored') = -1,
         'the not-scored sentinel was not preserved';
END $$;

-- The metrics join token_audit, which belongs to another family. Both tables
-- live in this store, so the join is ordinary SQL.
DO $$
DECLARE cost numeric; total bigint;
BEGIN
  DELETE FROM agent_log;
  INSERT INTO agent_log (id, role, agent_name, success) VALUES (500, 'exec', 'a1', true);
  INSERT INTO token_audit (agent_log_id, estimated_cost_usd, cache_read_tokens)
       VALUES (500, 2.50, 99);

  SELECT COALESCE(SUM(ta.estimated_cost_usd), 0), COUNT(*)
    INTO cost, total
    FROM agent_log al
    LEFT JOIN token_audit ta ON ta.agent_log_id = al.id
   WHERE al.role = 'exec';

  ASSERT cost = 2.50, format('the joined cost is %s, want 2.50', cost);
  ASSERT total = 1, format('the join produced %s rows, want 1', total);
END $$;

\echo 'AGENT-WORK FAMILY SUITE PASSED'
