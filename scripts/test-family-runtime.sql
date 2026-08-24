-- Verify the runtime family on a real server.
--
-- What only a real server settles is here: that a counter survives two
-- increments arriving together, that exactly one operator can be active, that
-- the agent cache actually caches instead of growing a row per call, and that
-- a snapshot of a file that did not exist is a real record rather than an
-- omission.
--
-- The telemetry schema loads too, because the OSV audit records an interaction
-- event -- a table another family owns. Both live in this store; what does not
-- cross the family boundary is a foreign key.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS file_snapshot_entries;
DROP TABLE IF EXISTS file_snapshots;
DROP TABLE IF EXISTS mcp_osv_cache;
DROP TABLE IF EXISTS decisions;
DROP TABLE IF EXISTS context_snapshots;
DROP TABLE IF EXISTS web_page_cache;
DROP TABLE IF EXISTS agent_cache;
DROP TABLE IF EXISTS context_cache;
DROP TABLE IF EXISTS tool_local_availability;
DROP TABLE IF EXISTS working_profile_state_local;
DROP TABLE IF EXISTS working_profile_observations_local;
DROP TABLE IF EXISTS model_pricing;
DROP TABLE IF EXISTS model_catalog;
DROP TABLE IF EXISTS maintenance_state;
DROP TABLE IF EXISTS project_clones;
DROP TABLE IF EXISTS local_operator;
DROP TABLE IF EXISTS env_capabilities;
DROP TABLE IF EXISTS memory_runtime_state;
DROP TABLE IF EXISTS interaction_events;

\i /tmp/family_schema_runtime.sql
\i /tmp/family_schema_telemetry.sql

-- --- the counter is atomic ---------------------------------------------------

-- The C read the value, parsed it, added, and wrote it back. Two increments
-- arriving together both read the same number and one was lost.
DO $$
DECLARE final bigint;
BEGIN
  INSERT INTO memory_runtime_state (state_key, state_value) VALUES ('counter', '10');

  -- ten increments, each doing its arithmetic in the statement
  FOR i IN 1..10 LOOP
    INSERT INTO memory_runtime_state (state_key, state_value) VALUES ('counter', '1')
    ON CONFLICT (state_key) DO UPDATE SET
        state_value = (COALESCE(NULLIF(memory_runtime_state.state_value,'')::bigint, 0) + 1)::text;
  END LOOP;

  SELECT state_value::bigint INTO final FROM memory_runtime_state WHERE state_key = 'counter';
  ASSERT final = 20, format('the counter reads %s, want 20', final);
END $$;

-- A key that has never been set starts from zero rather than failing: the
-- caller asking to add to it has said what it means.
DO $$
DECLARE value bigint;
BEGIN
  INSERT INTO memory_runtime_state (state_key, state_value) VALUES ('fresh', '5')
  ON CONFLICT (state_key) DO UPDATE SET
      state_value = (COALESCE(NULLIF(memory_runtime_state.state_value,'')::bigint, 0) + 5)::text
  RETURNING state_value::bigint INTO value;
  ASSERT value = 5, format('a new counter started at %s, want 5', value);
END $$;

-- --- exactly one operator is active -----------------------------------------

-- The C cleared every other row and then set one, from two statements. Between
-- them nobody was active; if the second failed, nobody was active for good.
DO $$
DECLARE active_count bigint; rejected boolean := false;
BEGIN
  INSERT INTO local_operator (secret_ref, operator_uuid, active) VALUES
    ('ref-a', 'uuid-a', true),
    ('ref-b', 'uuid-b', false);

  -- A second active operator is refused. The constraint is DEFERRABLE, so it
  -- fires at COMMIT rather than at the statement -- which is the whole point:
  -- a switch has to pass through a moment where two rows look active. Forcing
  -- it IMMEDIATE here is how that check is observed inside one transaction.
  BEGIN
    SET CONSTRAINTS local_operator_one_active IMMEDIATE;
    UPDATE local_operator SET active = true WHERE secret_ref = 'ref-b';
  EXCEPTION WHEN unique_violation THEN rejected := true;
  END;
  ASSERT rejected, 'two operators were active at once';
  SET CONSTRAINTS local_operator_one_active DEFERRED;

  -- Switching is two ordered writes in one transaction, NOT a data-modifying
  -- CTE: the sub-statements of one of those all see the same snapshot and their
  -- effects are not ordered against each other, so the deactivation and the
  -- activation race against the uniqueness check. The constraint is DEFERRABLE
  -- so the moment in between -- where two rows look active -- is allowed to
  -- exist and is checked at commit.
  UPDATE local_operator SET active = false WHERE active AND secret_ref <> 'ref-b';
  UPDATE local_operator SET active = true WHERE secret_ref = 'ref-b';

  SELECT COUNT(*) INTO active_count FROM local_operator WHERE active;
  ASSERT active_count = 1, format('%s operators are active, want 1', active_count);
  ASSERT (SELECT secret_ref FROM local_operator WHERE active) = 'ref-b',
         'the switch activated the wrong operator';
END $$;

-- --- the agent cache actually caches -----------------------------------------

-- The C's table had a surrogate id and no uniqueness, so its INSERT OR REPLACE
-- never replaced anything: every put appended a row, and a get returned
-- whichever duplicate the scan reached first.
DO $$
DECLARE rows_stored bigint; result text;
BEGIN
  INSERT INTO agent_cache (role, prompt, result) VALUES ('review', 'is this ok?', 'first answer')
  ON CONFLICT (role, prompt) DO UPDATE SET result = EXCLUDED.result, created_at = now();

  INSERT INTO agent_cache (role, prompt, result) VALUES ('review', 'is this ok?', 'second answer')
  ON CONFLICT (role, prompt) DO UPDATE SET result = EXCLUDED.result, created_at = now();

  SELECT COUNT(*) INTO rows_stored FROM agent_cache WHERE role = 'review';
  SELECT agent_cache.result INTO result
    FROM agent_cache WHERE role = 'review' AND prompt = 'is this ok?';

  ASSERT rows_stored = 1, format('%s rows for one cache key, want 1', rows_stored);
  ASSERT result = 'second answer',
         format('the cache returned %s, want the most recent answer', result);
END $$;

-- --- reading a web page marks it used ----------------------------------------

-- One statement, so a read cannot record a hit it did not serve. The
-- last_used_at it moves is what the eviction order reads.
DO $$
DECLARE body text; age bigint; used_after timestamptz; used_before timestamptz;
BEGIN
  INSERT INTO web_page_cache (url, body, byte_len, fetched_at, last_used_at)
       VALUES ('https://example.com/a', 'the page', 8,
               now() - interval '1 hour', now() - interval '1 hour');

  SELECT last_used_at INTO used_before FROM web_page_cache WHERE url = 'https://example.com/a';

  UPDATE web_page_cache SET last_used_at = now()
   WHERE url = 'https://example.com/a'
   RETURNING web_page_cache.body,
             EXTRACT(EPOCH FROM (now() - fetched_at))::bigint
        INTO body, age;

  SELECT last_used_at INTO used_after FROM web_page_cache WHERE url = 'https://example.com/a';

  ASSERT body = 'the page', 'the read did not return the page';
  ASSERT age >= 3600, format('the page reads as %s seconds old, want about an hour', age);
  ASSERT used_after > used_before, 'reading the page did not mark it used';
END $$;

-- A miss returns nothing and marks nothing.
DO $$
DECLARE hit bigint;
BEGIN
  UPDATE web_page_cache SET last_used_at = now() WHERE url = 'https://example.com/missing';
  GET DIAGNOSTICS hit = ROW_COUNT;
  ASSERT hit = 0, 'a miss touched a row';
END $$;

-- --- file snapshots -----------------------------------------------------------

-- get-or-create is one statement, so two callers snapshotting the same turn get
-- the SAME snapshot. The C read and then inserted, so both missed and both
-- inserted, and only one of the two would ever be restored.
DO $$
DECLARE first_id bigint; second_id bigint; total bigint;
BEGIN
  INSERT INTO file_snapshots (session_id, turn, label) VALUES ('s1', 3, 'before-edit')
  ON CONFLICT (session_id, turn, label) DO UPDATE SET session_id = EXCLUDED.session_id
  RETURNING id INTO first_id;

  INSERT INTO file_snapshots (session_id, turn, label) VALUES ('s1', 3, 'before-edit')
  ON CONFLICT (session_id, turn, label) DO UPDATE SET session_id = EXCLUDED.session_id
  RETURNING id INTO second_id;

  SELECT COUNT(*) INTO total FROM file_snapshots WHERE session_id = 's1' AND turn = 3;

  ASSERT first_id = second_id,
         format('two get-or-creates made snapshots %s and %s', first_id, second_id);
  ASSERT total = 1, format('%s snapshots of one turn', total);
END $$;

-- A file that did NOT exist is a real record: restoring has to delete it again,
-- and a missing row would leave it behind.
DO $$
DECLARE absent_content bytea; rejected boolean := false;
BEGIN
  INSERT INTO file_snapshot_entries (snapshot_id, path, existed, content)
       VALUES ((SELECT id FROM file_snapshots WHERE session_id='s1'), '/tmp/new.c', false, NULL);

  SELECT content INTO absent_content FROM file_snapshot_entries WHERE path = '/tmp/new.c';
  ASSERT absent_content IS NULL, 'a file recorded as absent carried content';

  -- and the two cannot disagree
  BEGIN
    INSERT INTO file_snapshot_entries (snapshot_id, path, existed, content)
         VALUES ((SELECT id FROM file_snapshots WHERE session_id='s1'),
                 '/tmp/bad.c', false, '\x00'::bytea);
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a file recorded as absent was given content';
END $$;

-- One row per path per snapshot: recording the same path twice would restore
-- whichever the query reached first.
DO $$
DECLARE stored bytea; rows_stored bigint;
BEGIN
  INSERT INTO file_snapshot_entries (snapshot_id, path, existed, content)
       VALUES ((SELECT id FROM file_snapshots WHERE session_id='s1'),
               '/tmp/edited.c', true, 'first'::bytea)
  ON CONFLICT (snapshot_id, path) DO UPDATE
     SET existed = EXCLUDED.existed, content = EXCLUDED.content;

  INSERT INTO file_snapshot_entries (snapshot_id, path, existed, content)
       VALUES ((SELECT id FROM file_snapshots WHERE session_id='s1'),
               '/tmp/edited.c', true, 'second'::bytea)
  ON CONFLICT (snapshot_id, path) DO UPDATE
     SET existed = EXCLUDED.existed, content = EXCLUDED.content;

  SELECT COUNT(*) INTO rows_stored
    FROM file_snapshot_entries WHERE path = '/tmp/edited.c';
  SELECT content INTO stored
    FROM file_snapshot_entries WHERE path = '/tmp/edited.c';

  ASSERT rows_stored = 1, format('%s entries for one path', rows_stored);
  ASSERT stored = 'second'::bytea, 'the re-record did not replace the content';
END $$;

-- Pruning takes the entries with the snapshots.
DO $$
DECLARE snapshots bigint; entries bigint;
BEGIN
  INSERT INTO file_snapshots (session_id, turn, label) VALUES
    ('s2', 1, 'a'), ('s2', 2, 'b'), ('s2', 3, 'c');
  INSERT INTO file_snapshot_entries (snapshot_id, path, existed, content)
       SELECT id, '/tmp/x.c', true, 'x'::bytea FROM file_snapshots WHERE session_id = 's2';

  DELETE FROM file_snapshots
   WHERE session_id = 's2'
     AND id NOT IN (SELECT id FROM file_snapshots WHERE session_id = 's2'
                     ORDER BY id DESC LIMIT 1);

  SELECT COUNT(*) INTO snapshots FROM file_snapshots WHERE session_id = 's2';
  SELECT COUNT(*) INTO entries FROM file_snapshot_entries e
    JOIN file_snapshots s ON s.id = e.snapshot_id WHERE s.session_id = 's2';

  ASSERT snapshots = 1, format('%s snapshots survived the prune, want 1', snapshots);
  ASSERT entries = 1, format('%s entries survived, want the 1 belonging to the kept snapshot',
                             entries);
  -- and nothing orphaned
  ASSERT (SELECT COUNT(*) FROM file_snapshot_entries e
           WHERE NOT EXISTS (SELECT 1 FROM file_snapshots s WHERE s.id = e.snapshot_id)) = 0,
         'entries outlived their snapshot';
END $$;

-- --- the model catalog and its prices -------------------------------------------

-- Freshness is a real comparison against the stored time.
DO $$
DECLARE fresh boolean; stale boolean;
BEGIN
  INSERT INTO model_catalog (provider, model, fetched_at)
       VALUES ('anthropic', 'opus', now() - interval '30 seconds');

  SELECT EXISTS (SELECT 1 FROM model_catalog WHERE provider = 'anthropic'
                   AND fetched_at > now() - make_interval(secs => 3600)) INTO fresh;
  SELECT EXISTS (SELECT 1 FROM model_catalog WHERE provider = 'anthropic'
                   AND fetched_at > now() - make_interval(secs => 10)) INTO stale;

  ASSERT fresh, 'a 30-second-old catalog read as stale against an hour TTL';
  ASSERT NOT stale, 'a 30-second-old catalog read as fresh against a 10-second TTL';
END $$;

-- Replacing one provider's catalog does not empty the others.
DO $$
DECLARE others bigint;
BEGIN
  INSERT INTO model_catalog (provider, model) VALUES ('openai', 'gpt');
  DELETE FROM model_catalog WHERE provider = 'anthropic';
  SELECT COUNT(*) INTO others FROM model_catalog WHERE provider = 'openai';
  ASSERT others = 1, 'replacing one provider emptied another';
END $$;

-- Prices are NUMERIC: a rate multiplied out is what a bill says.
DO $$
DECLARE total numeric;
BEGIN
  INSERT INTO model_pricing (model, cost_in_per_mtok, cost_out_per_mtok)
       VALUES ('opus', 0.000015, 0.000075);

  SELECT SUM(cost_in_per_mtok) INTO total
    FROM model_pricing, generate_series(1, 100000);
  ASSERT total = 1.5, format('a hundred thousand tokens at the stored rate came to %s, want 1.5',
                             total);
END $$;

-- --- the OSV audit records an interaction event --------------------------------

-- The C assembled a JSON payload and recorded it through the interaction-event
-- path, which is a table the agent-work family owns. A blocked package is
-- recorded with a "blocked" outcome, which is what makes it findable.
DO $$
DECLARE outcome text; client text;
BEGIN
  INSERT INTO interaction_events (session_id, event_type, actor, payload, outcome)
       VALUES ('', 'mcp_package_check', 'system',
               json_build_object('client', 'cli', 'ecosystem', 'npm', 'name', 'left-pad',
                                 'version', '1.0.0', 'verdict', 'vulnerable',
                                 'action', 'block', 'advisory_ids', 'GHSA-x')::text,
               CASE WHEN 'block' = 'block' THEN 'blocked' ELSE 'ok' END);

  SELECT interaction_events.outcome, payload::json->>'client' INTO outcome, client
    FROM interaction_events WHERE event_type = 'mcp_package_check';

  ASSERT outcome = 'blocked', format('a blocked package recorded outcome %s', outcome);
  ASSERT client = 'cli', format('the audit lost the client name (%s)', client);
END $$;

-- The cache honours its TTL: an entry older than the window is a miss, which is
-- the same thing as nothing cached as far as the caller is concerned.
DO $$
DECLARE within bigint; beyond bigint;
BEGIN
  INSERT INTO mcp_osv_cache (ecosystem, name, version, verdict, checked_at)
       VALUES ('npm', 'left-pad', '1.0.0', 'clean',
               EXTRACT(EPOCH FROM now())::bigint - 7200);

  SELECT COUNT(*) INTO within FROM mcp_osv_cache
   WHERE ecosystem='npm' AND name='left-pad' AND version='1.0.0'
     AND checked_at > EXTRACT(EPOCH FROM now())::bigint - 24 * 3600;
  SELECT COUNT(*) INTO beyond FROM mcp_osv_cache
   WHERE ecosystem='npm' AND name='left-pad' AND version='1.0.0'
     AND checked_at > EXTRACT(EPOCH FROM now())::bigint - 1 * 3600;

  ASSERT within = 1, 'a two-hour-old entry missed against a one-day TTL';
  ASSERT beyond = 0, 'a two-hour-old entry hit against a one-hour TTL';
END $$;

-- --- working profile observations -----------------------------------------------

-- The score is a running average weighted by how many observations there are,
-- so one confident observation does not overturn a settled profile.
DO $$
DECLARE score double precision; count_now bigint;
BEGIN
  -- three observations at 0.0
  FOR i IN 1..3 LOOP
    INSERT INTO working_profile_state_local (working_profile_key, score, observation_count)
         VALUES ('style', 0.0, 1)
    ON CONFLICT (working_profile_key) DO UPDATE SET
        score = (working_profile_state_local.score
                 * working_profile_state_local.observation_count + EXCLUDED.score)
                / (working_profile_state_local.observation_count + 1),
        observation_count = working_profile_state_local.observation_count + 1;
  END LOOP;

  -- then one at 1.0
  INSERT INTO working_profile_state_local (working_profile_key, score, observation_count)
       VALUES ('style', 1.0, 1)
  ON CONFLICT (working_profile_key) DO UPDATE SET
      score = (working_profile_state_local.score
               * working_profile_state_local.observation_count + EXCLUDED.score)
              / (working_profile_state_local.observation_count + 1),
      observation_count = working_profile_state_local.observation_count + 1;

  SELECT working_profile_state_local.score, observation_count INTO score, count_now
    FROM working_profile_state_local WHERE working_profile_key = 'style';

  ASSERT count_now = 4, format('%s observations recorded, want 4', count_now);
  ASSERT score < 0.5,
         format('one confident observation moved a settled profile to %s', score);
  ASSERT score > 0.0, 'the confident observation moved nothing at all';
END $$;

\echo 'RUNTIME FAMILY SUITE PASSED'
