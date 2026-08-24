-- Verify the workflow family on a real server.
--
-- What only a real server settles is here: that two workers racing for the same
-- roadmap unit produce exactly one winner, that re-dispatching a roadmap does
-- not forget when it started, that a plan's steps go with it, and that a lease
-- that was never set is not the same thing as one that ran out.

\set ON_ERROR_STOP on

-- Dependents first: pipelines, plan_steps, step_evidence and execution_trace
-- all reference execution_plans.
DROP TABLE IF EXISTS step_evidence;
DROP TABLE IF EXISTS plan_steps;
DROP TABLE IF EXISTS execution_trace;
DROP TABLE IF EXISTS pipelines;
DROP TABLE IF EXISTS execution_plans;
DROP TABLE IF EXISTS roadmap_unit_dispatch;
DROP TABLE IF EXISTS roadmap_dispatch;
DROP TABLE IF EXISTS workflow_binding;

\i /tmp/family_schema_workflow.sql

-- --- the unit claim decides between two workers -------------------------------

-- The C set claimed_by and state='active' on whatever row matched, with no
-- guard at all. Two coordinators that had both selected the same unit -- which
-- is the normal outcome of two of them running, since select_next is just a
-- read -- both "succeeded", and the second silently took the unit from the
-- first. Both then worked it.
--
-- claimed_by = '' in the WHERE is the same condition select_next uses to decide
-- a unit is available, so the claim itself becomes the decision.
DO $$
DECLARE first_won bigint; second_won bigint; owner text;
BEGIN
  INSERT INTO roadmap_unit_dispatch (roadmap_id, unit_id) VALUES ('rm-1', 'unit-1');

  UPDATE roadmap_unit_dispatch
     SET claimed_by = 'worker-a', claimed_at = now(), heartbeat_at = now(),
         state = 'active', dispatch_attempts = dispatch_attempts + 1, updated_at = now()
   WHERE roadmap_id = 'rm-1' AND unit_id = 'unit-1' AND claimed_by = '';
  GET DIAGNOSTICS first_won = ROW_COUNT;

  UPDATE roadmap_unit_dispatch
     SET claimed_by = 'worker-b', claimed_at = now(), heartbeat_at = now(),
         state = 'active', dispatch_attempts = dispatch_attempts + 1, updated_at = now()
   WHERE roadmap_id = 'rm-1' AND unit_id = 'unit-1' AND claimed_by = '';
  GET DIAGNOSTICS second_won = ROW_COUNT;

  SELECT claimed_by INTO owner
    FROM roadmap_unit_dispatch WHERE roadmap_id = 'rm-1' AND unit_id = 'unit-1';

  ASSERT first_won = 1, format('the first claimant changed %s rows, want 1', first_won);
  ASSERT second_won = 0, format('the second claimant changed %s rows, want 0', second_won);
  ASSERT owner = 'worker-a', format('the unit is held by %s, want the first claimant', owner);
END $$;

-- Finishing releases the unit, so it is claimable again and reads as unowned.
DO $$
DECLARE owner text; claimed timestamptz; reclaimed bigint;
BEGIN
  UPDATE roadmap_unit_dispatch
     SET state = 'done', result = 'ok', error = '',
         claimed_by = '', claimed_at = NULL, updated_at = now()
   WHERE roadmap_id = 'rm-1' AND unit_id = 'unit-1';

  SELECT claimed_by, claimed_at INTO owner, claimed
    FROM roadmap_unit_dispatch WHERE roadmap_id = 'rm-1' AND unit_id = 'unit-1';
  ASSERT owner = '' AND claimed IS NULL,
         'a finished unit still reads as claimed';
END $$;

-- A unit cannot be half-claimed: an owner without a time, or a time without an
-- owner, would let the selector hand out a unit somebody holds.
DO $$
DECLARE no_time boolean := false; no_owner boolean := false;
BEGIN
  BEGIN
    UPDATE roadmap_unit_dispatch SET claimed_by = 'ghost', claimed_at = NULL
     WHERE roadmap_id = 'rm-1' AND unit_id = 'unit-1';
  EXCEPTION WHEN check_violation THEN no_time := true;
  END;
  ASSERT no_time, 'a unit was claimed with no claim time';

  BEGIN
    UPDATE roadmap_unit_dispatch SET claimed_by = '', claimed_at = now()
     WHERE roadmap_id = 'rm-1' AND unit_id = 'unit-1';
  EXCEPTION WHEN check_violation THEN no_owner := true;
  END;
  ASSERT no_owner, 'a unit carried a claim time with no owner';
END $$;

-- Ensuring a unit that is already being worked on leaves it alone. A
-- coordinator re-walking the roadmap must not reset a live unit to pending.
DO $$
DECLARE state_now text; owner text;
BEGIN
  DELETE FROM roadmap_unit_dispatch;
  INSERT INTO roadmap_unit_dispatch (roadmap_id, unit_id, state, claimed_by, claimed_at)
       VALUES ('rm-2', 'unit-1', 'active', 'worker-a', now());

  INSERT INTO roadmap_unit_dispatch (roadmap_id, unit_id, level, state, tool_policy_mode)
       VALUES ('rm-2', 'unit-1', 'task', 'pending', 'execution')
  ON CONFLICT (roadmap_id, unit_id) DO NOTHING;

  SELECT state, claimed_by INTO state_now, owner
    FROM roadmap_unit_dispatch WHERE roadmap_id = 'rm-2' AND unit_id = 'unit-1';
  ASSERT state_now = 'active' AND owner = 'worker-a',
         format('ensuring reset a live unit to %s/%s', state_now, owner);
END $$;

-- --- re-dispatching a roadmap does not forget when it started ------------------

-- The C used INSERT OR REPLACE, which deletes the row and inserts a new one.
CREATE TEMP TABLE rd_probe (created_at timestamptz);

DO $$
BEGIN
  INSERT INTO roadmap_dispatch (roadmap_id, token_profile) VALUES ('rm-1', 'balanced');
  INSERT INTO rd_probe SELECT created_at FROM roadmap_dispatch WHERE roadmap_id = 'rm-1';
  UPDATE roadmap_dispatch SET status = 'stopped', phase = 'verify', exit_reason = 'paused'
   WHERE roadmap_id = 'rm-1';
END $$;

SELECT pg_sleep(0.05);

DO $$
DECLARE first_created timestamptz; now_created timestamptz;
        status_now text; phase_now text; reason text;
BEGIN
  SELECT created_at INTO first_created FROM rd_probe;

  INSERT INTO roadmap_dispatch (roadmap_id, status, phase, token_profile,
                                require_slice_discussion, budget_ceiling_tokens, exit_reason)
       VALUES ('rm-1', 'running', 'plan', 'thorough', true, 5000, '')
  ON CONFLICT (roadmap_id) DO UPDATE SET
      status = 'running', phase = 'plan',
      token_profile = EXCLUDED.token_profile,
      require_slice_discussion = EXCLUDED.require_slice_discussion,
      budget_ceiling_tokens = EXCLUDED.budget_ceiling_tokens,
      exit_reason = '', updated_at = now();

  SELECT created_at, status, phase, exit_reason
    INTO now_created, status_now, phase_now, reason
    FROM roadmap_dispatch WHERE roadmap_id = 'rm-1';

  -- re-dispatching DOES restart it
  ASSERT status_now = 'running' AND phase_now = 'plan' AND reason = '',
         'a re-dispatch did not restart the roadmap';
  -- but it does not rewrite when the roadmap was first started
  ASSERT now_created = first_created,
         'created_at moved on re-dispatch: how long the roadmap has been running is lost';
END $$;

-- --- plans and steps ------------------------------------------------------------

-- The C's queries had to match status '0' and '1' as well as the names, because
-- an older writer had put numbers in the column. A store that admits two
-- spellings for one state has two states.
DO $$
DECLARE rejected boolean := false;
BEGIN
  INSERT INTO execution_plans (id, task) VALUES (1, 'do the thing');
  INSERT INTO plan_steps (id, plan_id, seq, action) VALUES (1, 1, 0, 'step one');

  BEGIN
    UPDATE plan_steps SET status = '1' WHERE id = 1;
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a step status was set to a number';
END $$;

-- A step's position within its plan is unique, so "step 3" names one step.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO plan_steps (plan_id, seq, action) VALUES (1, 0, 'a second step zero');
  EXCEPTION WHEN unique_violation THEN rejected := true;
  END;
  ASSERT rejected, 'two steps share a position in the same plan';
END $$;

-- Cancelling only touches a plan that is still going: a plan that finished
-- between the caller's read and its write is not resurrected as cancelled.
DO $$
DECLARE changed bigint; status_now text;
BEGIN
  UPDATE execution_plans SET status = 'done' WHERE id = 1;

  UPDATE execution_plans
     SET status = 'cancelled', cancelled_at = now(), cancel_reason = 'too late'
   WHERE id = 1 AND status IN ('pending', 'running');
  GET DIAGNOSTICS changed = ROW_COUNT;

  SELECT status INTO status_now FROM execution_plans WHERE id = 1;
  ASSERT changed = 0, 'a finished plan was cancelled';
  ASSERT status_now = 'done', format('the plan reads as %s, want done', status_now);
END $$;

-- A cancelled plan says when; a plan that is not cancelled says nothing.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    UPDATE execution_plans SET status = 'cancelled', cancelled_at = NULL WHERE id = 1;
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a plan was cancelled with no cancellation time';
END $$;

-- Deleting a plan takes its steps and their evidence.
DO $$
DECLARE leftover bigint;
BEGIN
  INSERT INTO step_evidence (plan_id, step_id, kind, content)
       VALUES (1, 1, 'test', 'it passed');

  DELETE FROM execution_plans WHERE id = 1;
  SELECT (SELECT COUNT(*) FROM plan_steps WHERE plan_id = 1)
       + (SELECT COUNT(*) FROM step_evidence WHERE plan_id = 1) INTO leftover;
  ASSERT leftover = 0, format('%s rows outlived their plan', leftover);
END $$;

-- A trace OUTLIVES the plan it belonged to -- that is what a trace is for -- so
-- deleting the plan empties the link rather than taking the trace with it.
DO $$
DECLARE surviving bigint; still_linked bigint;
BEGIN
  INSERT INTO execution_plans (id, task) VALUES (2, 'another plan');
  INSERT INTO execution_trace (plan_id, session_id, turn, tool_name)
       VALUES (2, 's1', 0, 'Read');

  DELETE FROM execution_plans WHERE id = 2;

  SELECT COUNT(*) INTO surviving FROM execution_trace WHERE session_id = 's1';
  SELECT COUNT(*) INTO still_linked FROM execution_trace WHERE plan_id = 2;

  ASSERT surviving = 1, 'the trace was deleted with its plan';
  ASSERT still_linked = 0, 'the trace still names a plan that is gone';
  ASSERT (SELECT COALESCE(plan_id, 0) FROM execution_trace WHERE session_id = 's1') = 0,
         'an unlinked trace does not render as 0 on the wire';
END $$;

-- Orphaned steps are the ones still running under a plan that is not.
DO $$
DECLARE failed bigint;
BEGIN
  DELETE FROM plan_steps; DELETE FROM execution_plans;
  INSERT INTO execution_plans (id, task, status) VALUES
    (10, 'running plan', 'running'),
    (11, 'stopped plan', 'failed');
  INSERT INTO plan_steps (plan_id, seq, action, status) VALUES
    (10, 0, 'still going', 'running'),
    (11, 0, 'orphaned',    'running'),
    (11, 1, 'already done','done');

  UPDATE plan_steps s SET status = 'failed', finished_at = now()
    FROM execution_plans p
   WHERE p.id = s.plan_id AND s.status = 'running' AND p.status <> 'running';
  GET DIAGNOSTICS failed = ROW_COUNT;

  ASSERT failed = 1, format('failed %s steps, want the 1 orphan', failed);
  ASSERT (SELECT status FROM plan_steps WHERE plan_id = 10 AND seq = 0) = 'running',
         'a step under a running plan was failed';
END $$;

-- --- leases ----------------------------------------------------------------------

-- "No lease" and "a lease that ran out" are different things. The C spelled the
-- first as an empty string and compared it lexically against a formatted
-- timestamp, so it had to exclude '' by hand -- because '' sorts before every
-- date and would otherwise have read as long expired.
DO $$
DECLARE stale bigint; reclaimed bigint;
BEGIN
  INSERT INTO workflow_binding (aimee_session_id, work_item_id, lease_expiry) VALUES
    ('s-none',    'w-1', NULL),
    ('s-live',    'w-2', now() + interval '1 hour'),
    ('s-expired', 'w-3', now() - interval '1 second');

  SELECT COUNT(*) INTO stale FROM workflow_binding
   WHERE lease_expiry IS NOT NULL AND lease_expiry < now();
  ASSERT stale = 1, format('%s bindings read as stale, want the 1 that expired', stale);

  UPDATE workflow_binding SET lease_expiry = NULL, updated_at = now()
   WHERE lease_expiry IS NOT NULL AND lease_expiry < now();
  GET DIAGNOSTICS reclaimed = ROW_COUNT;
  ASSERT reclaimed = 1, format('reclaimed %s bindings, want 1', reclaimed);

  -- the live one is untouched
  ASSERT (SELECT lease_expiry IS NOT NULL FROM workflow_binding WHERE aimee_session_id = 's-live'),
         'a live lease was reclaimed';
END $$;

-- A ttl of 0 clears the lease; a negative one sets it in the past, which is how
-- a caller forces a binding stale on purpose.
DO $$
DECLARE cleared timestamptz; forced timestamptz;
BEGIN
  UPDATE workflow_binding
     SET lease_expiry = CASE WHEN 0 = 0 THEN NULL
                             ELSE now() + make_interval(secs => 0) END
   WHERE aimee_session_id = 's-live';
  SELECT lease_expiry INTO cleared FROM workflow_binding WHERE aimee_session_id = 's-live';
  ASSERT cleared IS NULL, 'a zero ttl did not clear the lease';

  UPDATE workflow_binding
     SET lease_expiry = now() + make_interval(secs => -60)
   WHERE aimee_session_id = 's-live';
  SELECT lease_expiry INTO forced FROM workflow_binding WHERE aimee_session_id = 's-live';
  ASSERT forced < now(), 'a negative ttl did not force the lease stale';
END $$;

-- Binding says whether it was new. xmax = 0 is how an upsert distinguishes an
-- insert from an update, which the caller needs to tell "bound" from "rebound".
DO $$
DECLARE was_new boolean; was_update boolean;
BEGIN
  INSERT INTO workflow_binding (aimee_session_id, work_item_id, enforce_stage)
       VALUES ('s-new', 'w-9', 'off')
  ON CONFLICT (aimee_session_id) DO UPDATE SET
      work_item_id = EXCLUDED.work_item_id, updated_at = now()
  RETURNING (xmax = 0) INTO was_new;

  INSERT INTO workflow_binding (aimee_session_id, work_item_id, enforce_stage)
       VALUES ('s-new', 'w-10', 'on')
  ON CONFLICT (aimee_session_id) DO UPDATE SET
      work_item_id = EXCLUDED.work_item_id, updated_at = now()
  RETURNING (xmax = 0) INTO was_update;

  ASSERT was_new, 'the first bind did not report as new';
  ASSERT NOT was_update, 'the rebind reported as new';
  ASSERT (SELECT work_item_id FROM workflow_binding WHERE aimee_session_id = 's-new') = 'w-10',
         'the rebind did not move the binding';
END $$;

\echo 'WORKFLOW FAMILY SUITE PASSED'
