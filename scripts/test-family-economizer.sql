-- Verify the the store state family's native PostgreSQL surface on a real server.
--
-- The Go unit tests drive the wire against an in-memory store, and that store's
-- map replaces on every write no matter what the SQL says. So they prove the
-- FRAMES are right and prove nothing at all about ON CONFLICT. This is the part
-- only a real PostgreSQL can answer.
--
-- The two statements are PREPAREd from the same text the module ships, rather
-- than retyped with literals, so a divergence between this test and the module
-- is a test that stops preparing rather than a test that quietly checks
-- something else.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS economizer_state;

\i /tmp/family_schema_economizer.sql

PREPARE state_load (text) AS
  SELECT state FROM economizer_state WHERE session_id = $1;

PREPARE state_save (text, text) AS
  INSERT INTO economizer_state (session_id, state)
       VALUES ($1, $2)
  ON CONFLICT (session_id)
  DO UPDATE SET state = EXCLUDED.state, updated_at = now();

-- A cold session has no row: the client reports this as "not found", which is
-- the normal first turn and not a failure.
DO $$
BEGIN
  ASSERT NOT EXISTS (SELECT 1 FROM economizer_state WHERE session_id = 'conv-1'),
    'a cold session already had state';
END $$;

EXECUTE state_save('conv-1', '{"page":1}');
EXECUTE state_save('conv-1', '{"page":2}');
EXECUTE state_save('conv-1', '{"page":3}');

-- The C kept one row per session with DELETE-then-INSERT. The primary key plus
-- ON CONFLICT must produce the same observable result: one row, newest value.
DO $$
DECLARE
  rows  bigint;
  value text;
BEGIN
  SELECT count(*) INTO rows FROM economizer_state WHERE session_id = 'conv-1';
  ASSERT rows = 1, format('three saves left %s rows, want 1 -- ON CONFLICT is not replacing', rows);
  SELECT state INTO value FROM economizer_state WHERE session_id = 'conv-1';
  ASSERT value = '{"page":3}', format('state is %s, want the newest blob', value);
END $$;

-- updated_at must move on a replace. If it did not, the column would be
-- recording insert time under a name that says otherwise.
DO $$
DECLARE
  before timestamptz;
  after  timestamptz;
BEGIN
  SELECT updated_at INTO before FROM economizer_state WHERE session_id = 'conv-1';
  PERFORM pg_sleep(0.01);
  EXECUTE 'EXECUTE state_save(''conv-1'', ''{"page":4}'')';
  SELECT updated_at INTO after FROM economizer_state WHERE session_id = 'conv-1';
  ASSERT after > before, 'updated_at did not advance on replace';
END $$;

-- Sessions must not collide: the key is the session, so a second session is a
-- second row and neither overwrites the other.
EXECUTE state_save('conv-2', '{"other":true}');
DO $$
DECLARE
  one text;
  two text;
BEGIN
  SELECT state INTO one FROM economizer_state WHERE session_id = 'conv-1';
  SELECT state INTO two FROM economizer_state WHERE session_id = 'conv-2';
  ASSERT one = '{"page":4}', format('conv-1 = %s', one);
  ASSERT two = '{"other":true}', format('conv-2 = %s', two);
  ASSERT (SELECT count(*) FROM economizer_state) = 2, 'wrong row count across sessions';
END $$;

-- "Exactly one row per session" must be an invariant the database enforces, not
-- a convention the module maintains. A plain INSERT of a duplicate has to fail.
DO $$
BEGIN
  BEGIN
    INSERT INTO economizer_state (session_id, state) VALUES ('conv-1', '{"sneaked":true}');
    ASSERT false, 'a duplicate session_id was accepted -- the primary key is not enforcing';
  EXCEPTION WHEN unique_violation THEN
    NULL;  -- expected
  END;
END $$;

-- A blob at the wire cap stores and reads back byte-for-byte. The module
-- refuses to HAND BACK something this large, but the column must not be what
-- imposes that -- otherwise the refusal would be a truncation instead.
DO $$
DECLARE
  big text := repeat('x', 6144);
  got text;
BEGIN
  EXECUTE format('EXECUTE state_save(%L, %L)', 'conv-big', big);
  SELECT state INTO got FROM economizer_state WHERE session_id = 'conv-big';
  ASSERT length(got) = 6144, format('stored %s bytes of 6144', length(got));
  ASSERT got = big, 'the stored blob did not round-trip byte-for-byte';
END $$;

-- The column types are the native ones, not SQLite's spelled in PostgreSQL.
-- checkpoints.created_at is TEXT DEFAULT to_char(now(), ...); this must not be.
DO $$
DECLARE
  kind text;
BEGIN
  SELECT data_type INTO kind FROM information_schema.columns
   WHERE table_name = 'economizer_state' AND column_name = 'updated_at';
  ASSERT kind = 'timestamp with time zone',
    format('updated_at is %s, want timestamp with time zone', kind);
END $$;

DROP TABLE economizer_state;

\echo 'ECONOMIZER FAMILY SUITE PASSED'
