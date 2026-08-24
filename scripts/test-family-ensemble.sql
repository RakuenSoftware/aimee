-- Verify the ensemble family on a real server.
--
-- The Go tests settle the state machine, which touches no database at all.
-- What only a real server settles is here: that the CHECKs admit exactly the
-- states the module writes, that the channel ordering picks the run a caller
-- means by "the current one", and that the row lock advance depends on really
-- serialises two turns arriving together.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS ensembles;

\i /tmp/family_schema_ensemble.sql

-- --- the status CHECK --------------------------------------------------------

-- A run that reads as something outside this set falls to the ELSE branch of
-- the channel ordering and is picked last forever without anything saying why.
DO $$
DECLARE rejected boolean := false;
BEGIN
  INSERT INTO ensembles (id, template_name, channel, status)
       VALUES (1, 'code-review', 'general', 'active');
  UPDATE ensembles SET status = 'complete' WHERE id = 1;

  BEGIN
    UPDATE ensembles SET status = 'running' WHERE id = 1;
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'the status CHECK admitted a state the module never writes';
END $$;

-- --- a paused run says why, and only a paused run carries a reason -----------

-- The C left stale reasons on rows it un-paused, so a reader could see an
-- active run still claiming it had been interrupted by a human.
DO $$
DECLARE no_reason boolean := false; stale_reason boolean := false;
BEGIN
  BEGIN
    UPDATE ensembles SET status = 'paused', paused_reason = '' WHERE id = 1;
  EXCEPTION WHEN check_violation THEN no_reason := true;
  END;
  ASSERT no_reason, 'a run paused for no stated reason was accepted';

  UPDATE ensembles SET status = 'paused', paused_reason = 'manual' WHERE id = 1;

  BEGIN
    UPDATE ensembles SET status = 'active' WHERE id = 1;
  EXCEPTION WHEN check_violation THEN stale_reason := true;
  END;
  ASSERT stale_reason, 'a run was un-paused while still carrying its pause reason';

  -- un-pausing properly clears it
  UPDATE ensembles SET status = 'active', paused_reason = '' WHERE id = 1;
  ASSERT (SELECT paused_reason FROM ensembles WHERE id = 1) = '',
         'the reason survived a proper un-pause';
END $$;

-- A position cannot be negative: the walk indexes the template with it.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    UPDATE ensembles SET current_phase = -1 WHERE id = 1;
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'a run was moved to a negative phase';
END $$;

-- --- the channel ordering ----------------------------------------------------

-- "The current ensemble for this channel" means the most usable one: an active
-- run before a paused one, a paused one before a completed one, and among
-- equals the one touched most recently.
DO $$
DECLARE current_id bigint;
BEGIN
  DELETE FROM ensembles;
  INSERT INTO ensembles (id, template_name, channel, status, paused_reason, updated_at) VALUES
    (10, 'code-review', 'dev', 'complete', '',        now()),
    (11, 'code-review', 'dev', 'paused',   'manual',  now()),
    (12, 'code-review', 'dev', 'active',   '',        now() - interval '1 hour'),
    (13, 'code-review', 'other', 'active', '',        now());

  SELECT id INTO current_id FROM ensembles
   WHERE channel = 'dev'
   ORDER BY CASE status WHEN 'active' THEN 0 WHEN 'paused' THEN 1
                        WHEN 'complete' THEN 2 ELSE 3 END,
            updated_at DESC, id DESC
   LIMIT 1;

  -- the active run wins even though it is the least recently touched, and the
  -- other channel's run is not a candidate at all
  ASSERT current_id = 12,
         format('the current run for dev is %s, want the active one (12)', current_id);
END $$;

-- With no active run the paused one is next, not the completed one.
DO $$
DECLARE current_id bigint;
BEGIN
  DELETE FROM ensembles WHERE id = 12;

  SELECT id INTO current_id FROM ensembles
   WHERE channel = 'dev'
   ORDER BY CASE status WHEN 'active' THEN 0 WHEN 'paused' THEN 1
                        WHEN 'complete' THEN 2 ELSE 3 END,
            updated_at DESC, id DESC
   LIMIT 1;

  ASSERT current_id = 11,
         format('with no active run the current one is %s, want the paused one (11)', current_id);
END $$;

-- --- the row lock ------------------------------------------------------------

-- advance is a read-modify-write: it reads the position and transcript, decides
-- the next turn from them, and writes both back. The C did that as a plain
-- SELECT and a plain UPDATE -- its functions were named _locked, but no lock of
-- any kind existed anywhere in the module. Two turns arriving together both
-- read position N, both appended to the transcript they had read, and the
-- second write erased the first message and re-took the same turn.
--
-- This uses a SECOND CONNECTION, because that is the only way to observe the
-- property at all: NOWAIT inside the same transaction always succeeds, since a
-- transaction already holds its own locks. A single-session version of this
-- test would pass whether or not the lock existed.
CREATE EXTENSION IF NOT EXISTS dblink;

DO $$
DECLARE blocked boolean := false;
BEGIN
  DELETE FROM ensembles;
  INSERT INTO ensembles (id, template_name, channel, status, context_json)
       VALUES (20, 'code-review', 'dev', 'active', '[]');
END $$;

-- Hold the row from this session, then have another connection try to take it.
BEGIN;
SELECT 1 FROM ensembles WHERE id = 20 FOR UPDATE;

DO $$
DECLARE blocked boolean := false;
BEGIN
  BEGIN
    PERFORM * FROM dblink('dbname=postgres',
                          'SELECT 1 FROM ensembles WHERE id = 20 FOR UPDATE NOWAIT')
                 AS probe(taken int);
  EXCEPTION WHEN OTHERS THEN blocked := true;
  END;
  ASSERT blocked,
         'a second session took a row this transaction holds FOR UPDATE: two turns '
         'arriving together would both read the same position and the second write '
         'would erase the first message';
END $$;

COMMIT;

-- And once the holder commits, the row is takeable again -- the lock is for the
-- duration of the turn, not the life of the run.
DO $$
DECLARE taken boolean := true;
BEGIN
  BEGIN
    PERFORM * FROM dblink('dbname=postgres',
                          'SELECT 1 FROM ensembles WHERE id = 20 FOR UPDATE NOWAIT')
                 AS probe(got int);
  EXCEPTION WHEN OTHERS THEN taken := false;
  END;
  ASSERT taken, 'the row stayed locked after the holding transaction committed';
END $$;

-- The transcript is a JSON array, and a turn appends to it rather than
-- replacing it. A malformed document would make every subsequent turn fail to
-- parse, so the column's shape is worth pinning.
DO $$
DECLARE turns bigint; last_sender text;
BEGIN
  UPDATE ensembles
     SET context_json = (context_json::jsonb ||
                         jsonb_build_object('sender','alice','text','a','phase',0,'turn',0))::text
   WHERE id = 20;
  UPDATE ensembles
     SET context_json = (context_json::jsonb ||
                         jsonb_build_object('sender','bob','text','b','phase',0,'turn',1))::text
   WHERE id = 20;

  SELECT jsonb_array_length(context_json::jsonb) INTO turns FROM ensembles WHERE id = 20;
  SELECT context_json::jsonb -> -1 ->> 'sender' INTO last_sender FROM ensembles WHERE id = 20;

  ASSERT turns = 2, format('the transcript holds %s turns, want 2', turns);
  ASSERT last_sender = 'bob', format('the last turn is %s, want bob', last_sender);
END $$;

\echo 'ENSEMBLE FAMILY SUITE PASSED'
