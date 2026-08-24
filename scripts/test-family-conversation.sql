-- Verify the conversation family on a real server.
--
-- The Go tests script the database, so they settle dispatch and validation and
-- say nothing about what the SQL actually does. What only a real server settles
-- is here: that the full-text search ranks before it truncates, that the term
-- match is both_sides about case, that the upsert preserves created_at, that the
-- prunes keep the rows they claim to, and that a chain cannot claim another
-- session's events.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS clarify_qa;
DROP TABLE IF EXISTS clarify_sessions;
DROP TABLE IF EXISTS user_memories;
DROP TABLE IF EXISTS window_files;
DROP TABLE IF EXISTS window_terms;
DROP TABLE IF EXISTS windows;
DROP TABLE IF EXISTS conv_context_state;
DROP TABLE IF EXISTS conv_tool_events;
DROP TABLE IF EXISTS conv_tool_chains;
DROP TABLE IF EXISTS payload_rewrite_state;
DROP TABLE IF EXISTS working_memory;

\i /tmp/family_schema_conversation.sql

-- --- working memory: the upsert preserves created_at -------------------------

-- The C used INSERT OR REPLACE, which deletes the row and inserts a new one, so
-- created_at was rewritten on every write and stopped meaning "created". A
-- caller reasoning about how long something had been held read the time of the
-- last touch instead.
--
-- The two writes are in SEPARATE transactions on purpose. now() is transaction
-- start time, so an insert and an update inside one block would stamp the same
-- instant and the test could not tell a preserved created_at from a rewritten
-- one.
CREATE TEMP TABLE wm_probe (created_at timestamptz);

DO $$
BEGIN
  INSERT INTO working_memory (session_id, key, value, category)
       VALUES ('s1', 'k', 'first', 'general');
  INSERT INTO wm_probe
       SELECT created_at FROM working_memory WHERE session_id='s1' AND key='k';
END $$;

SELECT pg_sleep(0.05);

DO $$
DECLARE first_created timestamptz; second_created timestamptz;
        second_updated timestamptz; v text;
BEGIN
  SELECT created_at INTO first_created FROM wm_probe;

  INSERT INTO working_memory (session_id, key, value, category, expires_at)
       VALUES ('s1', 'k', 'second', 'other', NULL)
  ON CONFLICT (session_id, key) DO UPDATE
     SET value = EXCLUDED.value, category = EXCLUDED.category,
         expires_at = EXCLUDED.expires_at, updated_at = now();

  SELECT created_at, updated_at, value
    INTO second_created, second_updated, v
    FROM working_memory WHERE session_id='s1' AND key='k';

  ASSERT v = 'second', format('value = %s, want the second write', v);
  ASSERT second_created = first_created,
         'created_at moved on update: it no longer means when the entry was created';
  ASSERT second_updated > first_created,
         'updated_at did not move on update';
  ASSERT (SELECT count(*) FROM working_memory WHERE session_id='s1') = 1,
         'the upsert made a second row instead of updating';
END $$;

-- --- working memory: the garbage collector counts what it deleted ------------

-- The C counted with one statement and deleted with another, so the number it
-- reported was what had expired when it looked, not what it removed.
DO $$
DECLARE removed bigint; survivors bigint;
BEGIN
  INSERT INTO working_memory (session_id, key, value, expires_at) VALUES
    ('gc', 'dead1', 'x', now() - interval '1 hour'),
    ('gc', 'dead2', 'x', now() - interval '1 second'),
    ('gc', 'alive', 'x', now() + interval '1 hour'),
    ('gc', 'forever', 'x', NULL);

  DELETE FROM working_memory
   WHERE expires_at IS NOT NULL AND expires_at <= now();
  GET DIAGNOSTICS removed = ROW_COUNT;

  SELECT count(*) INTO survivors FROM working_memory WHERE session_id='gc';
  ASSERT removed = 2, format('reported %s removed, want 2', removed);
  ASSERT survivors = 2, format('%s survivors, want the unexpired and the eternal', survivors);
END $$;

-- --- working memory: the search escapes the caller's text --------------------

-- The C built "%...%" by concatenation with no escaping, so a query containing
-- % matched everything rather than a literal percent sign.
DO $$
DECLARE literal bigint; wild bigint;
BEGIN
  INSERT INTO working_memory (session_id, key, value) VALUES
    ('esc-a', 'discount', 'saved 50% today'),
    ('esc-b', 'savings', 'we saved 500 dollars');

  -- searching for the literal "50%" must find exactly the one entry
  SELECT count(DISTINCT session_id) INTO literal
    FROM working_memory
   WHERE key ILIKE '%50\%%' ESCAPE '\' OR value ILIKE '%50\%%' ESCAPE '\';

  -- unescaped, the same text is a wildcard and matches both
  SELECT count(DISTINCT session_id) INTO wild
    FROM working_memory
   WHERE key ILIKE '%50%%' OR value ILIKE '%50%%';

  ASSERT literal = 1, format('the escaped search found %s sessions, want 1', literal);
  ASSERT wild = 2, 'the unescaped form did not over-match, so this test proves nothing';
END $$;

-- --- the term search is both_sides about case ---------------------------------

-- The C lowered the STORED term but bound the caller's term raw, so
-- LOWER(term) = 'Deploy' could never hold and a capitalised search found
-- nothing at all.
DO $$
DECLARE one_sided bigint; both_sides bigint;
BEGIN
  INSERT INTO windows (id, session_id, seq, summary, created_at)
       VALUES (1, 'sess', 1, 'a window about deployment', now());
  INSERT INTO window_terms (window_id, term) VALUES (1, 'Deploy');

  -- the C's shape: one side lowered
  SELECT count(*) INTO one_sided
    FROM window_terms WHERE lower(term) = ANY (ARRAY['Deploy']);

  -- both sides lowered
  SELECT count(*) INTO both_sides
    FROM window_terms
   WHERE lower(term) = ANY (SELECT lower(t) FROM unnest(ARRAY['Deploy']::text[]) AS t);

  ASSERT one_sided = 0,
         'the one-sided comparison matched, so this test no longer shows the defect';
  ASSERT both_sides = 1,
         format('the both_sides comparison found %s rows, want 1', both_sides);
END $$;

-- --- the full-text search ranks before it truncates ---------------------------

-- The C's statement had no ORDER BY and no LIMIT: it stopped reading after
-- `max` rows, so it returned the first `max` hits in rowid order -- an
-- arbitrary subset rather than the best matches. The window that mentions the
-- term most is inserted LAST here, so a search limited to one row returns it
-- only if the ranking happens before the limit.
DO $$
DECLARE best bigint; hits bigint;
BEGIN
  INSERT INTO windows (id, session_id, seq, summary, created_at) VALUES
    (10, 'fts', 1, 'a passing mention of postgres', now()),
    (11, 'fts', 2, 'unrelated text about weather',  now()),
    (12, 'fts', 3, 'postgres postgres postgres, thoroughly about postgres', now());

  SELECT w.id INTO best
    FROM windows w
    JOIN unnest(ARRAY['postgres']::text[]) AS t
      ON to_tsvector('english', w.summary) @@ plainto_tsquery('english', t)
   WHERE w.session_id = 'fts'
   GROUP BY w.id
   ORDER BY -MAX(ts_rank(to_tsvector('english', w.summary),
                         plainto_tsquery('english', t))) ASC, w.id
   LIMIT 1;

  ASSERT best = 12,
         format('a one-row search returned window %s, want the best match (12)', best);

  -- and the search finds both mentions, not just the strongest
  SELECT count(*) INTO hits FROM (
    SELECT w.id
      FROM windows w
      JOIN unnest(ARRAY['postgres']::text[]) AS t
        ON to_tsvector('english', w.summary) @@ plainto_tsquery('english', t)
     WHERE w.session_id = 'fts'
     GROUP BY w.id
  ) matched;
  ASSERT hits = 2, format('found %s windows mentioning the term, want 2', hits);
END $$;

-- The rank keeps FTS5's sign convention: more negative is a better match, so a
-- caller sorting ascending still gets its best hit first.
DO $$
DECLARE strong double precision; weak double precision;
BEGIN
  SELECT -MAX(ts_rank(to_tsvector('english', summary), plainto_tsquery('english', 'postgres')))
    INTO strong FROM windows WHERE id = 12;
  SELECT -MAX(ts_rank(to_tsvector('english', summary), plainto_tsquery('english', 'postgres')))
    INTO weak FROM windows WHERE id = 10;

  ASSERT strong < 0, format('the better match ranked %s, want a negative rank', strong);
  ASSERT strong < weak,
         format('the better match ranked %s and the weaker %s: the sign is inverted',
                strong, weak);
END $$;

-- The index is derived, so a summary rewritten WITHOUT any indexing call is
-- immediately searchable under its new text. Under the C this was the bug the
-- separate FTS table made possible: the index kept the old words until someone
-- remembered to call window_index_summary.
DO $$
DECLARE under_new bigint; under_old bigint;
BEGIN
  UPDATE windows SET summary = 'now entirely about kubernetes' WHERE id = 10;

  SELECT count(*) INTO under_new FROM windows
   WHERE id = 10 AND to_tsvector('english', summary) @@ plainto_tsquery('english', 'kubernetes');
  SELECT count(*) INTO under_old FROM windows
   WHERE id = 10 AND to_tsvector('english', summary) @@ plainto_tsquery('english', 'postgres');

  ASSERT under_new = 1, 'the rewritten summary is not searchable under its new text';
  ASSERT under_old = 0, 'the rewritten summary is still searchable under its old text';
END $$;

-- --- the prunes keep the rows they claim to ----------------------------------

-- window_terms keeps the LONGEST terms. The C ordered by SQLite's implicit
-- rowid to pick what to keep; there is no rowid here, so the identity column is
-- what makes the ordering a declared thing rather than an artefact.
DO $$
DECLARE kept text[];
BEGIN
  INSERT INTO windows (id, session_id, seq, summary, created_at)
       VALUES (20, 'prune', 1, '', now());
  INSERT INTO window_terms (window_id, term) VALUES
    (20, 'a'), (20, 'bb'), (20, 'cccc'), (20, 'ddddd'), (20, 'ee');

  DELETE FROM window_terms
   WHERE window_id = 20
     AND id NOT IN (SELECT id FROM window_terms WHERE window_id = 20
                     ORDER BY LENGTH(term) DESC, term LIMIT 2);

  SELECT array_agg(term ORDER BY term) INTO kept
    FROM window_terms WHERE window_id = 20;
  ASSERT kept = ARRAY['cccc','ddddd'],
         format('kept %s, want the two longest terms', kept);
END $$;

-- window_files keeps the OLDEST rows, so its ordering column matters more.
DO $$
DECLARE kept text[];
BEGIN
  INSERT INTO window_files (window_id, file_path) VALUES
    (20, 'first.c'), (20, 'second.c'), (20, 'third.c'), (20, 'fourth.c');

  DELETE FROM window_files
   WHERE window_id = 20
     AND id NOT IN (SELECT id FROM window_files WHERE window_id = 20
                     ORDER BY id LIMIT 2);

  SELECT array_agg(file_path ORDER BY id) INTO kept
    FROM window_files WHERE window_id = 20;
  ASSERT kept = ARRAY['first.c','second.c'],
         format('kept %s, want the two inserted first', kept);
END $$;

-- Deleting a window takes its terms and its files with it.
DO $$
DECLARE leftover bigint;
BEGIN
  DELETE FROM windows WHERE id = 20;
  SELECT (SELECT count(*) FROM window_terms WHERE window_id = 20)
       + (SELECT count(*) FROM window_files WHERE window_id = 20) INTO leftover;
  ASSERT leftover = 0, format('%s child rows outlived their window', leftover);
END $$;

-- --- a chain cannot claim another session's events ---------------------------

-- The C matched an id range and chain_id = 0 with NO session filter. Ids are
-- global, so a chain claimed whatever fell in the range, including another
-- session's events. The chain knows its own session, which is enough to scope
-- it without changing the wire.
DO $$
DECLARE claimed bigint; stolen bigint;
BEGIN
  INSERT INTO conv_tool_chains (id, session_id) VALUES (1, 'mine');
  INSERT INTO conv_tool_events (id, session_id, tool_name) VALUES
    (1, 'mine',   'Read'),
    (2, 'theirs', 'Read'),   -- interleaved, and inside the range below
    (3, 'mine',   'Write');

  UPDATE conv_tool_events
     SET chain_id = 1
   WHERE id BETWEEN 1 AND 3
     AND chain_id IS NULL
     AND session_id = (SELECT session_id FROM conv_tool_chains WHERE id = 1);
  GET DIAGNOSTICS claimed = ROW_COUNT;

  SELECT count(*) INTO stolen
    FROM conv_tool_events WHERE session_id = 'theirs' AND chain_id IS NOT NULL;

  ASSERT claimed = 2, format('claimed %s events, want the 2 belonging to the chain', claimed);
  ASSERT stolen = 0, 'the chain claimed an event belonging to another session';
END $$;

-- Deleting a chain returns its events to unassigned.
--
-- chain_id is NULL for "no chain" and the wire spells that 0, which is what
-- makes the reference possible at all: a foreign key holds for every non-null
-- value, and no chain has id 0. Without the reference these events kept
-- pointing at a chain that no longer existed, and no pass would ever pick them
-- up again.
DO $$
DECLARE dangling bigint; pending bigint; on_wire bigint;
BEGIN
  DELETE FROM conv_tool_chains WHERE id = 1;

  SELECT count(*) INTO dangling
    FROM conv_tool_events e
   WHERE e.chain_id IS NOT NULL
     AND NOT EXISTS (SELECT 1 FROM conv_tool_chains c WHERE c.id = e.chain_id);
  ASSERT dangling = 0,
         format('%s events point at a chain that no longer exists', dangling);

  -- and they are pending again, so the next pass finds them
  SELECT count(*) INTO pending
    FROM conv_tool_events WHERE session_id = 'mine' AND chain_id IS NULL;
  ASSERT pending = 2,
         format('%s events returned to unassigned, want the 2 the chain held', pending);

  -- the wire still sees the sentinel, not a null
  SELECT COALESCE(chain_id, 0) INTO on_wire FROM conv_tool_events WHERE id = 1;
  ASSERT on_wire = 0, format('an unassigned event renders as %s, want 0', on_wire);
END $$;

-- The reference also refuses an assignment to a chain that was never created.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    UPDATE conv_tool_events SET chain_id = 9999 WHERE id = 1;
  EXCEPTION WHEN foreign_key_violation THEN rejected := true;
  END;
  ASSERT rejected, 'an event was assigned to a chain that does not exist';
END $$;

-- --- clarify: the status CHECK admits exactly the module's three values ------

DO $$
DECLARE rejected boolean := false;
BEGIN
  INSERT INTO clarify_sessions (id, description, status) VALUES (1, 'a task', 'open');
  UPDATE clarify_sessions SET status = 'ready' WHERE id = 1;
  UPDATE clarify_sessions SET status = 'cancelled' WHERE id = 1;
  BEGIN
    UPDATE clarify_sessions SET status = 'finished' WHERE id = 1;
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'the status CHECK admitted a value the module never writes';
END $$;

-- Answering lands on the OLDEST outstanding question, which is the one the
-- caller was asked.
DO $$
DECLARE answered_id bigint; still_open bigint;
BEGIN
  INSERT INTO clarify_sessions (id, description) VALUES (2, 'another task');
  INSERT INTO clarify_qa (id, session_id, dimension, question, seq) VALUES
    (10, 2, 'scope',       'q1', 0),
    (11, 2, 'constraints', 'q2', 1);

  UPDATE clarify_qa SET answer = 'an answer', answered = 1
   WHERE id = (SELECT id FROM clarify_qa
                WHERE session_id = 2 AND answered = 0
                ORDER BY seq ASC, id ASC LIMIT 1);

  SELECT id INTO answered_id FROM clarify_qa WHERE session_id = 2 AND answered = 1;
  SELECT count(*) INTO still_open FROM clarify_qa WHERE session_id = 2 AND answered = 0;

  ASSERT answered_id = 10, format('the answer landed on pair %s, want the oldest', answered_id);
  ASSERT still_open = 1, format('%s questions still open, want 1', still_open);
END $$;

-- Deleting a clarify session takes its pairs.
DO $$
DECLARE leftover bigint;
BEGIN
  DELETE FROM clarify_sessions WHERE id = 2;
  SELECT count(*) INTO leftover FROM clarify_qa WHERE session_id = 2;
  ASSERT leftover = 0, format('%s pairs outlived their session', leftover);
END $$;

-- --- user memories: an upsert revives a retired memory -----------------------

DO $$
DECLARE state text; c double precision; rows bigint;
BEGIN
  INSERT INTO user_memories (kind, tier, key, content, confidence, lifecycle_state)
       VALUES ('fact', 'L2', 'name:user', 'old', 0.5, 'retired');

  INSERT INTO user_memories (kind, tier, key, content, confidence, source_session, updated_at)
       VALUES ('fact', 'L3', 'name:user', 'new', 0.9, 's1', now())
  ON CONFLICT (kind, key) DO UPDATE
     SET content = EXCLUDED.content, tier = EXCLUDED.tier,
         confidence = EXCLUDED.confidence, source_session = EXCLUDED.source_session,
         lifecycle_state = 'active', updated_at = now();

  SELECT lifecycle_state, confidence INTO state, c
    FROM user_memories WHERE kind='fact' AND key='name:user';
  SELECT count(*) INTO rows FROM user_memories WHERE key='name:user';

  ASSERT state = 'active', format('lifecycle_state = %s, want the memory revived', state);
  ASSERT c = 0.9, format('confidence = %s, want the new value', c);
  ASSERT rows = 1, format('%s rows for one key, want the upsert to have updated', rows);
END $$;

-- The identity recall selects on key prefix; a preference must not appear in it.
DO $$
DECLARE identity_hits bigint;
BEGIN
  INSERT INTO user_memories (kind, tier, key, content) VALUES
    ('fact',       'L2', 'role:engineer',  'x'),
    ('fact',       'L2', 'weather:sunny',  'x'),
    ('preference', 'L2', 'identity:theme', 'x');

  SELECT count(*) INTO identity_hits
    FROM user_memories
   WHERE tier IN ('L2','L3','L4','L5')
     AND lifecycle_state = 'active'
     AND (valid_until IS NULL OR valid_until > now())
     AND kind = 'fact'
     AND (key LIKE 'identity:%' OR key LIKE 'name:%' OR key LIKE 'role:%'
       OR key LIKE 'user:%' OR key LIKE 'self:%');

  -- name:user and role:engineer qualify; weather:sunny is the wrong prefix and
  -- identity:theme is the wrong kind
  ASSERT identity_hits = 2,
         format('the identity recall returned %s rows, want 2', identity_hits);
END $$;

-- An expired memory is not recalled.
--
-- The C consulted valid_until nowhere at all: nothing wrote it and nothing read
-- it, so a memory given an expiry was recalled forever regardless. An
-- unconsulted expiry is worse than an absent one -- the caller that set it
-- believes the fact stops being asserted, and the store keeps asserting it.
DO $$
DECLARE recalled bigint; lapsed_visible bigint;
BEGIN
  INSERT INTO user_memories (kind, tier, key, content, valid_until) VALUES
    ('fact', 'L2', 'self:lapsed',  'no longer true', now() - interval '1 day'),
    ('fact', 'L2', 'self:current', 'still true',     now() + interval '1 day'),
    ('fact', 'L2', 'self:eternal', 'always true',    NULL);

  SELECT count(*) INTO recalled
    FROM user_memories
   WHERE tier IN ('L2','L3','L4','L5')
     AND lifecycle_state = 'active'
     AND (valid_until IS NULL OR valid_until > now())
     AND kind = 'fact'
     AND key LIKE 'self:%';

  ASSERT recalled = 2,
         format('recalled %s of the self: memories, want the unexpired and the eternal', recalled);

  -- the lapsed one is still THERE; it is simply no longer asserted
  SELECT count(*) INTO lapsed_visible
    FROM user_memories WHERE key = 'self:lapsed';
  ASSERT lapsed_visible = 1, 'the expired memory was deleted rather than left unasserted';
END $$;

-- Reviving a memory clears a lapsed expiry. Setting lifecycle_state = 'active'
-- while leaving the old valid_until in place would make it active and still
-- invisible, which is the most confusing state the table can hold.
DO $$
DECLARE state text; expiry timestamptz; recalled bigint;
BEGIN
  INSERT INTO user_memories (kind, tier, key, content, confidence, source_session, updated_at)
       VALUES ('fact', 'L2', 'self:lapsed', 'true again', 1.0, 's2', now())
  ON CONFLICT (kind, key) DO UPDATE
     SET content = EXCLUDED.content, tier = EXCLUDED.tier,
         confidence = EXCLUDED.confidence, source_session = EXCLUDED.source_session,
         lifecycle_state = 'active', valid_until = NULL, updated_at = now();

  SELECT lifecycle_state, valid_until INTO state, expiry
    FROM user_memories WHERE kind='fact' AND key='self:lapsed';

  SELECT count(*) INTO recalled
    FROM user_memories
   WHERE lifecycle_state = 'active'
     AND (valid_until IS NULL OR valid_until > now())
     AND key = 'self:lapsed';

  ASSERT state = 'active', format('lifecycle_state = %s after revival', state);
  ASSERT expiry IS NULL, 'the revived memory kept its lapsed expiry';
  ASSERT recalled = 1, 'the revived memory is active but still not recalled';
END $$;

\echo 'CONVERSATION FAMILY SUITE PASSED'
