-- Verify the guardrail session-state family on a real server.
--
-- The Go tests script the database, so what only a real server settles is here:
-- that a signed BIGINT round-trips the full unsigned hash range, that deleting a
-- session actually cascades to all six child tables, and that the expiry window
-- is measured the way the module assumes.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS session_state_seen_paths;
DROP TABLE IF EXISTS session_state_read_paths;
DROP TABLE IF EXISTS session_state_worktrees;
DROP TABLE IF EXISTS session_state_tdd_writes;
DROP TABLE IF EXISTS session_state_ap_hits;
DROP TABLE IF EXISTS session_state_file_hashes;
DROP TABLE IF EXISTS session_state;

\i /tmp/family_schema_guardrail.sql

-- --- the unsigned hash in a signed column -------------------------------------

-- content_hash arrives unsigned (the C prints %llu) and is stored as the BITS in
-- a signed BIGINT. Both directions must be exact, including above 2^63-1 where
-- the stored value is negative.
DO $$
DECLARE
  stored bigint;
  back   numeric;
BEGIN
  INSERT INTO session_state (session_id) VALUES ('sess-hash');
  -- 2^64-1 as the module stores it
  INSERT INTO session_state_file_hashes VALUES ('sess-hash', '/f', -1);
  SELECT content_hash INTO stored FROM session_state_file_hashes WHERE session_id = 'sess-hash';
  ASSERT stored = -1, format('stored %s', stored);

  -- reinterpreting those bits as unsigned gives back the original value
  back := stored::numeric + 18446744073709551616::numeric;
  ASSERT back = 18446744073709551615::numeric,
    format('reinterpreted to %s, want 2^64-1', back);

  -- and a value below 2^63 stays positive
  INSERT INTO session_state_file_hashes VALUES ('sess-hash', '/g', 4611686018427387904);
  SELECT content_hash INTO stored FROM session_state_file_hashes WHERE path = '/g';
  ASSERT stored = 4611686018427387904, format('stored %s', stored);
END $$;

-- --- deleting a session takes its children with it ----------------------------

-- The module's delete is ONE statement. That is only correct if every child
-- table cascades; a table that did not would leave orphans no operation can
-- reach or clean up.
DO $$
DECLARE leftover bigint;
BEGIN
  INSERT INTO session_state (session_id) VALUES ('sess-cascade');
  INSERT INTO session_state_seen_paths  VALUES ('sess-cascade', 0, '/a');
  INSERT INTO session_state_read_paths  VALUES ('sess-cascade', 0, '/b');
  INSERT INTO session_state_worktrees   VALUES ('sess-cascade', 0, '/root', '/wt');
  INSERT INTO session_state_tdd_writes  VALUES ('sess-cascade', 0, 'stem', true);
  INSERT INTO session_state_ap_hits     VALUES ('sess-cascade', 1, 3);
  INSERT INTO session_state_file_hashes VALUES ('sess-cascade', '/c', 7);

  DELETE FROM session_state WHERE session_id = 'sess-cascade';

  SELECT
    (SELECT count(*) FROM session_state_seen_paths  WHERE session_id = 'sess-cascade') +
    (SELECT count(*) FROM session_state_read_paths  WHERE session_id = 'sess-cascade') +
    (SELECT count(*) FROM session_state_worktrees   WHERE session_id = 'sess-cascade') +
    (SELECT count(*) FROM session_state_tdd_writes  WHERE session_id = 'sess-cascade') +
    (SELECT count(*) FROM session_state_ap_hits     WHERE session_id = 'sess-cascade') +
    (SELECT count(*) FROM session_state_file_hashes WHERE session_id = 'sess-cascade')
  INTO leftover;
  ASSERT leftover = 0, format('%s child rows survived the delete', leftover);
END $$;

-- A child row for a session that does not exist must be impossible: the module
-- writes the parent first, and the foreign key is what guarantees the order
-- cannot silently be wrong.
DO $$
BEGIN
  BEGIN
    INSERT INTO session_state_seen_paths VALUES ('no-such-session', 0, '/a');
    ASSERT false, 'a child row was accepted for a session that does not exist';
  EXCEPTION WHEN foreign_key_violation THEN NULL; END;
END $$;

-- --- the snapshot rewrite ------------------------------------------------------

-- Save clears each child table and refills it. Two saves in a row must leave
-- only the second one's rows: the collections are a snapshot, not a log.
DO $$
DECLARE n bigint;
BEGIN
  INSERT INTO session_state (session_id) VALUES ('sess-snap');
  INSERT INTO session_state_seen_paths VALUES ('sess-snap', 0, '/first'), ('sess-snap', 1, '/second');

  DELETE FROM session_state_seen_paths WHERE session_id = 'sess-snap';
  INSERT INTO session_state_seen_paths VALUES ('sess-snap', 0, '/only');

  SELECT count(*) INTO n FROM session_state_seen_paths WHERE session_id = 'sess-snap';
  ASSERT n = 1, format('the rewrite left %s rows, want 1', n);
  ASSERT (SELECT path FROM session_state_seen_paths WHERE session_id = 'sess-snap') = '/only',
    'the rewrite kept the wrong row';
END $$;

-- The (session_id, seq) key is what keeps a collection ordered and unique.
DO $$
BEGIN
  BEGIN
    INSERT INTO session_state_seen_paths VALUES ('sess-snap', 0, '/duplicate-seq');
    ASSERT false, 'two paths shared one seq';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;

-- --- expiry --------------------------------------------------------------------

-- The caller supplies an AGE and the database supplies the instant, so a caller
-- whose clock has drifted cannot expire sessions early.
DO $$
DECLARE found text[];
BEGIN
  DELETE FROM session_state;
  INSERT INTO session_state (session_id, updated_at)
       VALUES ('old',    now() - interval '2 hours'),
              ('recent', now() - interval '1 minute');

  SELECT array_agg(session_id ORDER BY session_id) INTO found
    FROM session_state
   WHERE updated_at < now() - make_interval(secs => 3600);
  ASSERT found = ARRAY['old'], format('expired set = %s, want just the old one', found);
END $$;

-- A threshold of zero expires everything already written, which is what a
-- caller asking for "idle at all" means.
DO $$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n FROM session_state
   WHERE updated_at < now() - make_interval(secs => 0);
  ASSERT n = 2, format('a zero threshold expired %s of 2', n);
END $$;

-- The expiry index must exist: without it the sweep scans every session.
DO $$
DECLARE definition text;
BEGIN
  SELECT indexdef INTO definition FROM pg_indexes
   WHERE tablename = 'session_state' AND indexname = 'idx_session_state_updated';
  ASSERT definition IS NOT NULL, 'the expiry index is missing';
END $$;

-- --- the parent upsert ---------------------------------------------------------

DO $$
DECLARE mode text; n bigint;
BEGIN
  DELETE FROM session_state;
  INSERT INTO session_state (session_id, session_mode) VALUES ('sess-up', 'implement');
  INSERT INTO session_state (session_id, session_mode) VALUES ('sess-up', 'review')
    ON CONFLICT (session_id) DO UPDATE SET session_mode = EXCLUDED.session_mode,
                                           updated_at = now();
  SELECT session_mode INTO mode FROM session_state WHERE session_id = 'sess-up';
  ASSERT mode = 'review', format('session_mode = %s', mode);
  SELECT count(*) INTO n FROM session_state;
  ASSERT n = 1, format('the upsert appended: %s rows', n);
END $$;

DROP TABLE session_state_seen_paths;
DROP TABLE session_state_read_paths;
DROP TABLE session_state_worktrees;
DROP TABLE session_state_tdd_writes;
DROP TABLE session_state_ap_hits;
DROP TABLE session_state_file_hashes;
DROP TABLE session_state;

\echo 'GUARDRAIL FAMILY SUITE PASSED'
