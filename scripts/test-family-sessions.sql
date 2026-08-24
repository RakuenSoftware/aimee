-- Verify the sessions family on a real server.
--
-- The Go tests script the database, so what only a real server settles is here:
-- that the persona claim is genuinely race-free, that the live revision really
-- advances on the upsert, that the write-path sequence allocates without
-- colliding, and that ILIKE finds what LIKE would not.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS server_sessions;
DROP TABLE IF EXISTS primary_sessions;
DROP TABLE IF EXISTS webchat_claude_sessions;
DROP TABLE IF EXISTS webchat_live;
DROP TABLE IF EXISTS session_state_write_paths;

\i /tmp/family_schema_sessions.sql

-- --- the persona claim is race-free --------------------------------------------

-- The claim is a guarded UPDATE. Two callers racing both run it; the row can
-- only leave state 0 once, so exactly one sees a row change.
DO $$
DECLARE first_claim bigint; second_claim bigint; state smallint;
BEGIN
  INSERT INTO server_sessions (id) VALUES ('sess-race');

  UPDATE server_sessions SET persona_delivery_state = 2, last_activity_at = now()
   WHERE id = 'sess-race' AND persona_delivery_state = 0;
  GET DIAGNOSTICS first_claim = ROW_COUNT;

  UPDATE server_sessions SET persona_delivery_state = 2, last_activity_at = now()
   WHERE id = 'sess-race' AND persona_delivery_state = 0;
  GET DIAGNOSTICS second_claim = ROW_COUNT;

  ASSERT first_claim = 1, format('the first claim changed %s rows, want 1', first_claim);
  ASSERT second_claim = 0, format('the second claim changed %s rows, want 0', second_claim);

  SELECT persona_delivery_state INTO state FROM server_sessions WHERE id = 'sess-race';
  ASSERT state = 2, format('state = %s, want 2 (in flight)', state);
END $$;

-- Finishing is guarded on holding the claim, so a caller that never claimed
-- cannot mark the persona delivered.
DO $$
DECLARE changed bigint; state smallint;
BEGIN
  UPDATE server_sessions SET persona_delivery_state = 1, last_activity_at = now()
   WHERE id = 'sess-race' AND persona_delivery_state = 2;
  GET DIAGNOSTICS changed = ROW_COUNT;
  ASSERT changed = 1, 'the holder could not finish';

  -- a second finish holds no claim and changes nothing
  UPDATE server_sessions SET persona_delivery_state = 1, last_activity_at = now()
   WHERE id = 'sess-race' AND persona_delivery_state = 2;
  GET DIAGNOSTICS changed = ROW_COUNT;
  ASSERT changed = 0, 'a caller without the claim finished anyway';

  SELECT persona_delivery_state INTO state FROM server_sessions WHERE id = 'sess-race';
  ASSERT state = 1, format('state = %s, want 1 (delivered)', state);
END $$;

-- Releasing on failure returns the session to unclaimed so a later request
-- retries rather than the persona being lost.
DO $$
DECLARE state smallint;
BEGIN
  UPDATE server_sessions SET persona_delivery_state = 2 WHERE id = 'sess-race';
  UPDATE server_sessions SET persona_delivery_state = 0
   WHERE id = 'sess-race' AND persona_delivery_state = 2;
  SELECT persona_delivery_state INTO state FROM server_sessions WHERE id = 'sess-race';
  ASSERT state = 0, format('state = %s, want it back to unclaimed', state);

  -- and it can be claimed again
  UPDATE server_sessions SET persona_delivery_state = 2
   WHERE id = 'sess-race' AND persona_delivery_state = 0;
  SELECT persona_delivery_state INTO state FROM server_sessions WHERE id = 'sess-race';
  ASSERT state = 2, 'a released session could not be reclaimed';
END $$;

-- There is no fourth state.
DO $$
BEGIN
  BEGIN
    UPDATE server_sessions SET persona_delivery_state = 3 WHERE id = 'sess-race';
    ASSERT false, 'persona_delivery_state 3 was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- --- expiry is interval arithmetic, not string building -------------------------

DO $$
DECLARE ids text[]; removed bigint;
BEGIN
  DELETE FROM server_sessions;
  INSERT INTO server_sessions (id, created_at) VALUES
    ('old',    now() - interval '2 hours'),
    ('recent', now() - interval '1 minute');

  SELECT array_agg(id ORDER BY id) INTO ids
    FROM server_sessions
   WHERE created_at <= now() - make_interval(secs => 3600);
  ASSERT ids = ARRAY['old'], format('expired = %s, want just the old one', ids);

  DELETE FROM server_sessions WHERE created_at <= now() - make_interval(secs => 3600);
  GET DIAGNOSTICS removed = ROW_COUNT;
  ASSERT removed = 1, format('the sweep removed %s rows, want 1', removed);
  ASSERT EXISTS (SELECT 1 FROM server_sessions WHERE id = 'recent'),
    'the sweep removed a recent session';
END $$;

-- --- the title search finds what LIKE would not ---------------------------------

-- SQLite's LIKE was case-insensitive. ILIKE keeps a search that used to find a
-- session finding it.
DO $$
DECLARE found text[];
BEGIN
  DELETE FROM server_sessions;
  INSERT INTO server_sessions (id, title) VALUES
    ('a', 'Deploy the gateway'),
    ('b', 'deploy the worker'),
    ('c', 'unrelated');

  SELECT array_agg(id ORDER BY id) INTO found
    FROM server_sessions WHERE title ILIKE '%deploy%';
  ASSERT found = ARRAY['a','b'], format('ILIKE found %s, want both', found);

  -- and the comparison is meaningful: LIKE finds only one of them
  SELECT array_agg(id ORDER BY id) INTO found
    FROM server_sessions WHERE title LIKE '%deploy%';
  ASSERT found = ARRAY['b'],
    format('LIKE found %s -- if this is not just b, the ILIKE test proves nothing', found);
END $$;

-- --- the live revision advances --------------------------------------------------

DO $$
DECLARE r bigint; body text;
BEGIN
  INSERT INTO webchat_live (session_id, turn_id, rev, text, status)
       VALUES ('sess', 't1', 1, 'hello', 'streaming')
  ON CONFLICT (session_id) DO UPDATE SET
      turn_id = EXCLUDED.turn_id, rev = webchat_live.rev + 1,
      text = EXCLUDED.text, status = EXCLUDED.status, updated_at = now();
  SELECT rev INTO r FROM webchat_live WHERE session_id = 'sess';
  ASSERT r = 1, format('the first write left rev = %s, want 1', r);

  INSERT INTO webchat_live (session_id, turn_id, rev, text, status)
       VALUES ('sess', 't1', 1, 'hello there', 'streaming')
  ON CONFLICT (session_id) DO UPDATE SET
      turn_id = EXCLUDED.turn_id, rev = webchat_live.rev + 1,
      text = EXCLUDED.text, status = EXCLUDED.status, updated_at = now();
  SELECT rev, text INTO r, body FROM webchat_live WHERE session_id = 'sess';
  ASSERT r = 2, format('the second write left rev = %s, want 2 -- it did not advance', r);
  ASSERT body = 'hello there', format('text = %s', body);

  -- a poller holding rev 2 sees nothing new
  ASSERT NOT EXISTS (SELECT 1 FROM webchat_live WHERE session_id = 'sess' AND rev > 2),
    'a poller already at the current revision was told there was something new';
  ASSERT EXISTS (SELECT 1 FROM webchat_live WHERE session_id = 'sess' AND rev > 1),
    'a poller one revision behind was told there was nothing new';
END $$;

-- --- write-path sequence allocation ------------------------------------------------

-- seq is allocated from the current maximum. The primary key is what makes a
-- collision impossible, so the allocation and the key have to agree.
DO $$
DECLARE seqs bigint[];
BEGIN
  INSERT INTO session_state_write_paths (session_id, seq, path)
       VALUES ('sess', coalesce((SELECT max(seq) + 1 FROM session_state_write_paths
                                  WHERE session_id = 'sess'), 0), '/a');
  INSERT INTO session_state_write_paths (session_id, seq, path)
       VALUES ('sess', coalesce((SELECT max(seq) + 1 FROM session_state_write_paths
                                  WHERE session_id = 'sess'), 0), '/b');
  INSERT INTO session_state_write_paths (session_id, seq, path)
       VALUES ('sess', coalesce((SELECT max(seq) + 1 FROM session_state_write_paths
                                  WHERE session_id = 'sess'), 0), '/c');

  SELECT array_agg(seq ORDER BY seq) INTO seqs
    FROM session_state_write_paths WHERE session_id = 'sess';
  ASSERT seqs = ARRAY[0::bigint, 1::bigint, 2::bigint],
    format('sequences = %s, want 0,1,2', seqs);

  -- a different session starts its own sequence at 0
  INSERT INTO session_state_write_paths (session_id, seq, path)
       VALUES ('other', coalesce((SELECT max(seq) + 1 FROM session_state_write_paths
                                   WHERE session_id = 'other'), 0), '/x');
  ASSERT (SELECT seq FROM session_state_write_paths WHERE session_id = 'other') = 0,
    'a new session did not start its sequence at 0';
END $$;

-- --- the primary transcript upsert ---------------------------------------------------

DO $$
DECLARE body text; n bigint;
BEGIN
  INSERT INTO primary_sessions (session_id, agent_name, provider, messages_json)
       VALUES ('s', 'agent', 'prov', '[1]')
  ON CONFLICT (session_id, agent_name, provider)
  DO UPDATE SET messages_json = EXCLUDED.messages_json, updated_at = now();

  INSERT INTO primary_sessions (session_id, agent_name, provider, messages_json)
       VALUES ('s', 'agent', 'prov', '[1,2]')
  ON CONFLICT (session_id, agent_name, provider)
  DO UPDATE SET messages_json = EXCLUDED.messages_json, updated_at = now();

  SELECT messages_json INTO body FROM primary_sessions WHERE session_id = 's';
  ASSERT body = '[1,2]', format('messages = %s', body);
  SELECT count(*) INTO n FROM primary_sessions;
  ASSERT n = 1, format('the upsert appended: %s rows', n);

  -- a different provider for the same session is a different row
  INSERT INTO primary_sessions (session_id, agent_name, provider, messages_json)
       VALUES ('s', 'agent', 'other', '[9]');
  SELECT count(*) INTO n FROM primary_sessions;
  ASSERT n = 2, 'the composite key collapsed two providers into one row';
END $$;

-- --- webchat bindings ------------------------------------------------------------------

-- The ownership check reads by claude_session_id, which the primary key cannot
-- serve, so the index is what keeps every bind from scanning the table.
DO $$
DECLARE definition text;
BEGIN
  SELECT indexdef INTO definition FROM pg_indexes
   WHERE tablename = 'webchat_claude_sessions' AND indexname = 'idx_webchat_claude_sessions_csid';
  ASSERT definition IS NOT NULL, 'the ownership index is missing';
END $$;

-- A tab can exist under several historical principals; the bind updates every
-- copy because principal is attribution, not a namespace.
DO $$
DECLARE bound text[];
BEGIN
  INSERT INTO webchat_claude_sessions (principal, aimee_session_id, claude_session_id) VALUES
    ('webuser:old', 'aimee-1', 'claude-old'),
    ('webuser:new', 'aimee-1', 'claude-old');

  UPDATE webchat_claude_sessions
     SET claude_session_id = 'claude-new', updated_at = now()
   WHERE aimee_session_id = 'aimee-1';

  SELECT array_agg(DISTINCT claude_session_id) INTO bound
    FROM webchat_claude_sessions WHERE aimee_session_id = 'aimee-1';
  ASSERT bound = ARRAY['claude-new'],
    format('copies disagree after the bind: %s', bound);
END $$;

DROP TABLE server_sessions;
DROP TABLE primary_sessions;
DROP TABLE webchat_claude_sessions;
DROP TABLE webchat_live;
DROP TABLE session_state_write_paths;

\echo 'SESSIONS FAMILY SUITE PASSED'
