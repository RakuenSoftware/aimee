-- Verify the telemetry family on a real server.
--
-- What only a real server settles is here: that the idempotency index is part
-- of the schema rather than something a caller had to ask for, that money adds
-- up, that the pivots produce a row when there is nothing to pivot, and that
-- the hypothesis ranking ranks before it truncates.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS diagnosis_items;
DROP TABLE IF EXISTS diagnoses;
DROP TABLE IF EXISTS eval_results;
DROP TABLE IF EXISTS guardrail_events;
DROP TABLE IF EXISTS interaction_events;
DROP TABLE IF EXISTS cost_fold_log;
DROP TABLE IF EXISTS token_audit;

\i /tmp/family_schema_telemetry.sql

-- --- the idempotency guard is part of the schema -----------------------------

-- The C created this index from a SEPARATE operation the caller had to remember
-- to call. A store nobody had asked accepted duplicate charges silently, and
-- the window before that call is exactly when a retry storm arrives.
DO $$
DECLARE charges bigint; total numeric;
BEGIN
  INSERT INTO token_audit (source, principal, idempotency_key, attempt, estimated_cost_usd)
       VALUES ('anthropic', 'agent-1', 'req-abc', 0, 1.25)
  ON CONFLICT DO NOTHING;

  -- the same charge arriving again
  INSERT INTO token_audit (source, principal, idempotency_key, attempt, estimated_cost_usd)
       VALUES ('anthropic', 'agent-1', 'req-abc', 0, 1.25)
  ON CONFLICT DO NOTHING;

  SELECT COUNT(*), COALESCE(SUM(estimated_cost_usd), 0) INTO charges, total
    FROM token_audit WHERE idempotency_key = 'req-abc';

  ASSERT charges = 1, format('the same charge was recorded %s times', charges);
  ASSERT total = 1.25, format('the ledger says %s, want 1.25', total);
END $$;

-- A RETRY of the same request is a different attempt, and is its own charge.
DO $$
DECLARE charges bigint;
BEGIN
  INSERT INTO token_audit (source, principal, idempotency_key, attempt, estimated_cost_usd)
       VALUES ('anthropic', 'agent-1', 'req-abc', 1, 1.25);
  SELECT COUNT(*) INTO charges FROM token_audit WHERE idempotency_key = 'req-abc';
  ASSERT charges = 2, format('a second attempt did not record separately (%s rows)', charges);
END $$;

-- Rows with no idempotency key are not deduplicated against each other: a
-- caller that supplied no key asked for no guarantee.
DO $$
DECLARE charges bigint;
BEGIN
  INSERT INTO token_audit (source, principal, estimated_cost_usd) VALUES
    ('anthropic', 'agent-2', 0.10),
    ('anthropic', 'agent-2', 0.10);
  SELECT COUNT(*) INTO charges FROM token_audit WHERE principal = 'agent-2';
  ASSERT charges = 2, format('unkeyed charges were deduplicated (%s rows)', charges);
END $$;

-- --- money adds up -----------------------------------------------------------

-- estimated_cost_usd is NUMERIC, not a binary float. A spend total summed from
-- a few thousand charges is what a bill gets reconciled against, and binary
-- floating point does not add up to what the individual charges say.
DO $$
DECLARE total numeric; as_float double precision;
BEGIN
  DELETE FROM token_audit;
  INSERT INTO token_audit (source, principal, estimated_cost_usd)
       SELECT 'anthropic', 'sum-test', 0.1 FROM generate_series(1, 1000);

  SELECT SUM(estimated_cost_usd) INTO total FROM token_audit WHERE principal = 'sum-test';
  SELECT SUM(estimated_cost_usd::double precision) INTO as_float
    FROM token_audit WHERE principal = 'sum-test';

  ASSERT total = 100.0, format('a thousand charges of 0.10 summed to %s, want exactly 100', total);
  -- and the same sum in binary floating point does NOT land on it, which is
  -- why the column is not a double
  ASSERT as_float <> 100.0,
         'the float sum landed exactly, so this test no longer shows why NUMERIC matters';
END $$;

-- --- the pivots answer even with nothing to pivot -----------------------------

-- The C read a GROUP BY and filled whichever half it saw, so a session with
-- only supervisor turns left the worker half at whatever the struct was
-- initialised to. An aggregate always produces a row, so both halves are always
-- answered.
DO $$
DECLARE s_calls bigint; w_calls bigint;
BEGIN
  DELETE FROM token_audit;
  INSERT INTO token_audit (session_id, delegation_id, usage_kind, prompt_tokens) VALUES
    ('s1', '',        'realized', 100),
    ('s1', '',        'realized', 200);

  SELECT COUNT(*) FILTER (WHERE delegation_id = ''),
         COUNT(*) FILTER (WHERE delegation_id <> '')
    INTO s_calls, w_calls
    FROM token_audit
   WHERE session_id = 's1' AND (usage_kind = 'realized' OR usage_kind = '');

  ASSERT s_calls = 2, format('supervisor calls = %s, want 2', s_calls);
  ASSERT w_calls = 0, format('worker calls = %s, want 0', w_calls);
END $$;

-- A session with no rows at all still answers, with zeros.
DO $$
DECLARE s_calls bigint; rows_returned bigint;
BEGIN
  SELECT COUNT(*) FILTER (WHERE delegation_id = '') INTO s_calls
    FROM token_audit WHERE session_id = 'no-such-session';
  GET DIAGNOSTICS rows_returned = ROW_COUNT;
  ASSERT s_calls = 0, format('an unknown session reported %s calls', s_calls);
END $$;

-- An unrecognised usage_kind is counted as money SPENT, never dropped. A charge
-- that appears in no column at all is worse than one in the wrong column.
DO $$
DECLARE realized numeric; estimated numeric;
BEGIN
  DELETE FROM token_audit;
  INSERT INTO token_audit (source, usage_kind, estimated_cost_usd) VALUES
    ('a', 'realized',   1.00),
    ('a', '',           2.00),
    ('a', 'something',  4.00),
    ('a', 'estimated',  8.00);

  SELECT COALESCE(SUM(estimated_cost_usd) FILTER (
             WHERE usage_kind NOT IN ('estimated', 'avoided', 'partial')), 0),
         COALESCE(SUM(estimated_cost_usd) FILTER (WHERE usage_kind = 'estimated'), 0)
    INTO realized, estimated FROM token_audit;

  ASSERT realized = 7.00,
         format('realized spend is %s, want 7.00 (1 + 2 legacy + 4 unknown)', realized);
  ASSERT estimated = 8.00, format('estimated is %s, want 8.00', estimated);
END $$;

-- --- cost folding ------------------------------------------------------------

DO $$
DECLARE self_rejected boolean := false; total numeric;
BEGIN
  BEGIN
    INSERT INTO cost_fold_log (parent_session_id, child_session_id, cost_usd)
         VALUES ('s1', 's1', 5.00);
  EXCEPTION WHEN check_violation THEN self_rejected := true;
  END;
  ASSERT self_rejected, 'a session folded its cost into itself, doubling every total';

  -- a repeated fold updates rather than double-counting
  INSERT INTO cost_fold_log (parent_session_id, child_session_id, cost_usd)
       VALUES ('parent', 'child', 5.00);
  INSERT INTO cost_fold_log (parent_session_id, child_session_id, cost_usd)
       VALUES ('parent', 'child', 7.00)
  ON CONFLICT (parent_session_id, child_session_id) DO UPDATE
     SET cost_usd = EXCLUDED.cost_usd;

  SELECT SUM(cost_usd) INTO total FROM cost_fold_log WHERE parent_session_id = 'parent';
  ASSERT total = 7.00, format('the folded total is %s, want the updated 7.00', total);
END $$;

-- --- interaction events -------------------------------------------------------

-- Promotion follows reflection: an event cannot be promoted out of a feed it
-- was never reflected into. The C stamped promoted_at by id with no such check.
DO $$
DECLARE rejected boolean := false;
BEGIN
  INSERT INTO interaction_events (id, session_id, event_type) VALUES (1, 's1', 'tool_use');
  BEGIN
    UPDATE interaction_events SET promoted_at = now() WHERE id = 1;
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'an event was promoted without ever having been reflected';

  UPDATE interaction_events SET reflected_at = now() WHERE id = 1;
  UPDATE interaction_events SET promoted_at = now() WHERE id = 1;
  ASSERT (SELECT promoted_at IS NOT NULL FROM interaction_events WHERE id = 1),
         'a reflected event could not then be promoted';
END $$;

-- Marking is one statement over a whole batch, and it reports what it changed.
-- An event already marked is not marked twice.
DO $$
DECLARE marked bigint;
BEGIN
  INSERT INTO interaction_events (id, session_id, event_type) VALUES
    (2, 's1', 'tool_use'), (3, 's1', 'tool_use'), (4, 's1', 'tool_use');

  UPDATE interaction_events SET reflected_at = now()
   WHERE id = ANY (ARRAY[1, 2, 3]::bigint[]) AND reflected_at IS NULL;
  GET DIAGNOSTICS marked = ROW_COUNT;

  -- id 1 was already reflected above, so only 2 and 3 change
  ASSERT marked = 2, format('marked %s events, want the 2 that were unreflected', marked);
END $$;

-- --- evaluation results --------------------------------------------------------

DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO eval_results (suite, tool_calls, tool_call_failures)
         VALUES ('unit', 3, 5);
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a run failed more tool calls than it made';
END $$;

-- The recent-failure list is DISTINCT ON, so a task that failed the same way
-- twice appears once, ordered by its most recent failure.
DO $$
DECLARE names text[];
BEGIN
  INSERT INTO eval_results (suite, task_name, success, error, created_at) VALUES
    ('unit', 'alpha', false, 'boom', now() - interval '2 days'),
    ('unit', 'alpha', false, 'boom', now() - interval '1 day'),
    ('unit', 'beta',  false, 'bang', now() - interval '3 hours'),
    ('unit', 'gamma', true,  '',     now()),
    ('unit', 'delta', false, 'old',  now() - interval '30 days');

  SELECT array_agg(task_name ORDER BY task_name) INTO names
    FROM (SELECT DISTINCT ON (task_name, error) task_name, error, created_at
            FROM eval_results
           WHERE NOT success AND created_at > now() - interval '7 days'
           ORDER BY task_name, error, created_at DESC) recent;

  ASSERT names = ARRAY['alpha','beta'],
         format('recent failures are %s, want alpha and beta once each', names);
END $$;

-- --- diagnoses -----------------------------------------------------------------

-- A concluded diagnosis says what it concluded: concluding with nothing is
-- indistinguishable from never having concluded.
DO $$
DECLARE rejected boolean := false;
BEGIN
  INSERT INTO diagnoses (id, symptom) VALUES (1, 'the thing is slow');
  BEGIN
    UPDATE diagnoses SET status = 'concluded' WHERE id = 1;
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a diagnosis concluded with no conclusion';
END $$;

-- Evidence bears on a hypothesis; nothing else does. Evidence with no parent is
-- counted toward no hypothesis and is invisible to the ranking, which is the
-- same as not having been recorded.
DO $$
DECLARE orphan_rejected boolean := false; parented_rejected boolean := false;
BEGIN
  INSERT INTO diagnosis_items (id, diagnosis_id, kind, content)
       VALUES (10, 1, 'hypothesis', 'the cache is cold');

  BEGIN
    INSERT INTO diagnosis_items (diagnosis_id, kind, content)
         VALUES (1, 'evidence_for', 'evidence about nothing');
  EXCEPTION WHEN check_violation THEN orphan_rejected := true;
  END;
  ASSERT orphan_rejected, 'evidence was recorded bearing on no hypothesis';

  BEGIN
    INSERT INTO diagnosis_items (diagnosis_id, kind, parent_id, content)
         VALUES (1, 'observation', 10, 'an observation with a parent');
  EXCEPTION WHEN check_violation THEN parented_rejected := true;
  END;
  ASSERT parented_rejected, 'an observation was hung off a hypothesis';
END $$;

-- Deleting a hypothesis takes the evidence that bore on it: evidence about a
-- hypothesis nobody holds any more is evidence about nothing.
DO $$
DECLARE leftover bigint;
BEGIN
  INSERT INTO diagnosis_items (id, diagnosis_id, kind, parent_id, content, evidence_rank)
       VALUES (11, 1, 'evidence_for', 10, 'the cache miss rate spiked', 1);

  DELETE FROM diagnosis_items WHERE id = 10;
  SELECT COUNT(*) INTO leftover FROM diagnosis_items WHERE id = 11;
  ASSERT leftover = 0, 'evidence outlived the hypothesis it bore on';
END $$;

-- --- the ranking ranks before it truncates -------------------------------------

-- The C read hypotheses ordered by id, CUT the list to the caller's max, and
-- only then counted evidence and sorted. So asking for the top hypothesis
-- returned the OLDEST one. The best-supported hypothesis is inserted LAST here,
-- so it comes back only if the ranking happens before the limit.
DO $$
DECLARE best bigint; best_confidence double precision;
BEGIN
  DELETE FROM diagnosis_items;
  DELETE FROM diagnoses;
  INSERT INTO diagnoses (id, symptom) VALUES (2, 'requests time out');

  INSERT INTO diagnosis_items (id, diagnosis_id, kind, content) VALUES
    (20, 2, 'hypothesis', 'oldest and unsupported'),
    (21, 2, 'hypothesis', 'also unsupported'),
    (22, 2, 'hypothesis', 'newest and well supported');

  -- direct evidence, the strongest kind, for the newest hypothesis only
  INSERT INTO diagnosis_items (diagnosis_id, kind, parent_id, content, evidence_rank) VALUES
    (2, 'evidence_for', 22, 'reproduced in a controlled test', 1),
    (2, 'evidence_for', 22, 'the traces agree', 1);

  WITH weighted AS (
      SELECT h.id,
             COALESCE(SUM(
                 CASE WHEN e.kind = 'evidence_for' THEN 1 ELSE -1 END *
                 CASE LEAST(GREATEST(e.evidence_rank, 1), 4)
                      WHEN 1 THEN 1.0 WHEN 2 THEN 0.6 WHEN 3 THEN 0.3 ELSE 0.1 END
             ), 0) AS score
        FROM diagnosis_items h
        LEFT JOIN diagnosis_items e
               ON e.parent_id = h.id
              AND e.kind IN ('evidence_for', 'evidence_against')
       WHERE h.diagnosis_id = 2 AND h.kind = 'hypothesis'
       GROUP BY h.id
  )
  SELECT id, 1.0 / (1.0 + exp(-LEAST(GREATEST(score, -20.0), 20.0)))
    INTO best, best_confidence
    FROM weighted ORDER BY 2 DESC, id ASC LIMIT 1;

  ASSERT best = 22,
         format('the top hypothesis is %s, want the best-supported one (22)', best);
  ASSERT best_confidence > 0.5,
         format('the best-supported hypothesis scored %s, want above even', best_confidence);
END $$;

-- Evidence AGAINST pulls a hypothesis below even, so contradiction is not just
-- absence of support.
DO $$
DECLARE against_confidence double precision;
BEGIN
  INSERT INTO diagnosis_items (diagnosis_id, kind, parent_id, content, evidence_rank)
       VALUES (2, 'evidence_against', 20, 'ruled out by a direct test', 1);

  WITH weighted AS (
      SELECT h.id,
             COALESCE(SUM(
                 CASE WHEN e.kind = 'evidence_for' THEN 1 ELSE -1 END *
                 CASE LEAST(GREATEST(e.evidence_rank, 1), 4)
                      WHEN 1 THEN 1.0 WHEN 2 THEN 0.6 WHEN 3 THEN 0.3 ELSE 0.1 END
             ), 0) AS score
        FROM diagnosis_items h
        LEFT JOIN diagnosis_items e
               ON e.parent_id = h.id
              AND e.kind IN ('evidence_for', 'evidence_against')
       WHERE h.diagnosis_id = 2 AND h.kind = 'hypothesis'
       GROUP BY h.id
  )
  SELECT 1.0 / (1.0 + exp(-LEAST(GREATEST(score, -20.0), 20.0)))
    INTO against_confidence FROM weighted WHERE id = 20;

  ASSERT against_confidence < 0.5,
         format('a contradicted hypothesis scored %s, want below even', against_confidence);
END $$;

-- An evidence rank outside the scale is refused rather than stored: the ranking
-- reads it as a weight, and a weight nobody defined would silently become the
-- weakest one.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO diagnosis_items (diagnosis_id, kind, parent_id, content, evidence_rank)
         VALUES (2, 'evidence_for', 22, 'off the scale', 9);
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'an evidence rank outside the scale was stored';
END $$;

\echo 'TELEMETRY FAMILY SUITE PASSED'
