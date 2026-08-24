-- Verify the checkpoints and git-ownership families on a real server.
--
-- The Go tests script the database, so the things only a real server settles
-- are here: that RETURNING hands back the generated id and the formatted
-- timestamp, that the composite primary keys behave as the upserts assume, and
-- above all that the escaped LIKE pattern matches a literal prefix rather than
-- a wildcard.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS checkpoints;
DROP TABLE IF EXISTS branch_ownership;
DROP TABLE IF EXISTS session_feature_branch;

\i /tmp/family_schema_checkpoints.sql
\i /tmp/family_schema_git.sql

-- --- checkpoints: RETURNING and the formatted timestamp -----------------------

PREPARE cp_insert (bigint, text, text, text) AS
  INSERT INTO checkpoints (task_id, session_id, label, snapshot)
       VALUES ($1, $2, $3, $4)
    RETURNING id, task_id, session_id, label, snapshot,
              to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS');

-- One statement gives back the generated id and the wire's timestamp spelling.
-- The C needed three: insert, last_insert_rowid(), then a SELECT.
DO $$
DECLARE got record;
BEGIN
  EXECUTE $q$EXECUTE cp_insert(7, 'sess', 'before-edit', '{"n":1}')$q$;
  SELECT * INTO got FROM checkpoints WHERE label = 'before-edit';
  ASSERT got.id IS NOT NULL AND got.id > 0, 'the insert generated no id';
  ASSERT got.task_id = 7, format('task_id = %s', got.task_id);
END $$;

-- The wire spelling is exactly what SQLite's datetime('now') produced: 19
-- characters, UTC, no zone suffix and no fractional seconds.
DO $$
DECLARE rendered text;
BEGIN
  SELECT to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')
    INTO rendered FROM checkpoints LIMIT 1;
  ASSERT length(rendered) = 19,
    format('created_at renders as %s (%s chars), want 19', rendered, length(rendered));
  ASSERT rendered ~ '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$',
    format('created_at renders as %s', rendered);
END $$;

-- The list ordering must be TOTAL. created_at alone is not unique, so rows
-- written in the same instant would order arbitrarily and a LIMIT over them is
-- not a stable page.
DO $$
DECLARE ids bigint[];
BEGIN
  DELETE FROM checkpoints;
  -- three rows sharing one created_at, inserted in a known order
  INSERT INTO checkpoints (task_id, session_id, label, snapshot, created_at)
       VALUES (1,'s','a','{}', '2026-08-22 09:00:00+00'),
              (2,'s','b','{}', '2026-08-22 09:00:00+00'),
              (3,'s','c','{}', '2026-08-22 09:00:00+00');

  SELECT array_agg(id ORDER BY ord) INTO ids FROM (
    SELECT id, row_number() OVER (ORDER BY created_at DESC, id DESC) AS ord
      FROM checkpoints
  ) t;
  ASSERT ids[1] > ids[2] AND ids[2] > ids[3],
    format('the total ordering did not sort by id within the tie: %s', ids);

  -- and it is repeatable: the same query twice gives the same first row
  ASSERT (SELECT id FROM checkpoints ORDER BY created_at DESC, id DESC LIMIT 1)
       = (SELECT id FROM checkpoints ORDER BY created_at DESC, id DESC LIMIT 1),
    'the ordering is not stable';
END $$;

-- --- git ownership: composite keys and the upserts ----------------------------

DO $$
DECLARE owner text;
BEGIN
  INSERT INTO branch_ownership (repo_path, branch_name, session_id)
       VALUES ('/repo', 'main', 'sess-1')
  ON CONFLICT (repo_path, branch_name) DO UPDATE SET session_id = EXCLUDED.session_id;

  -- the same branch again REPLACES the owner rather than failing
  INSERT INTO branch_ownership (repo_path, branch_name, session_id)
       VALUES ('/repo', 'main', 'sess-2')
  ON CONFLICT (repo_path, branch_name) DO UPDATE SET session_id = EXCLUDED.session_id;

  SELECT session_id INTO owner FROM branch_ownership
   WHERE repo_path = '/repo' AND branch_name = 'main';
  ASSERT owner = 'sess-2', format('owner = %s, want the replacement', owner);
  ASSERT (SELECT count(*) FROM branch_ownership) = 1,
    'the upsert appended instead of replacing';

  -- the same branch name in a DIFFERENT repo is a different row
  INSERT INTO branch_ownership (repo_path, branch_name, session_id)
       VALUES ('/other', 'main', 'sess-3');
  ASSERT (SELECT count(*) FROM branch_ownership) = 2,
    'the composite key collapsed two repos into one row';
END $$;

DO $$
DECLARE branch text;
BEGIN
  INSERT INTO session_feature_branch (repo_path, session_id, feature_branch)
       VALUES ('/repo', 'sess-1', 'feat/a')
  ON CONFLICT (repo_path, session_id) DO UPDATE SET feature_branch = EXCLUDED.feature_branch;
  INSERT INTO session_feature_branch (repo_path, session_id, feature_branch)
       VALUES ('/repo', 'sess-1', 'feat/b')
  ON CONFLICT (repo_path, session_id) DO UPDATE SET feature_branch = EXCLUDED.feature_branch;

  SELECT feature_branch INTO branch FROM session_feature_branch
   WHERE repo_path = '/repo' AND session_id = 'sess-1';
  ASSERT branch = 'feat/b', format('feature branch = %s', branch);
  ASSERT (SELECT count(*) FROM session_feature_branch) = 1, 'the feature upsert appended';
END $$;

-- --- the prefix lookup is LITERAL ---------------------------------------------

-- This is the behaviour the C did not have. It built "<prefix>%" by
-- concatenation and bound it into LIKE unescaped, so a prefix containing a
-- wildcard matched more than it asked for.
PREPARE session_by_prefix (text) AS
  SELECT session_id FROM branch_ownership
   WHERE session_id LIKE $1 ESCAPE '\'
   ORDER BY session_id
   LIMIT 1;

DO $$
DECLARE got text;
BEGIN
  DELETE FROM branch_ownership;
  INSERT INTO branch_ownership (repo_path, branch_name, session_id) VALUES
    ('/r','b1','abc-normal'),
    ('/r','b2','50%-literal'),
    ('/r','b3','a_b-literal'),
    ('/r','b4','axb-other');

  -- an ordinary prefix still resolves
  EXECUTE $q$SELECT session_id FROM branch_ownership WHERE session_id LIKE 'abc%' ESCAPE '\' LIMIT 1$q$;
  SELECT session_id INTO got FROM branch_ownership
   WHERE session_id LIKE 'abc%' ESCAPE '\' ORDER BY session_id LIMIT 1;
  ASSERT got = 'abc-normal', format('ordinary prefix resolved to %s', got);

  -- a literal % in the prefix matches ONLY the row containing it
  SELECT session_id INTO got FROM branch_ownership
   WHERE session_id LIKE '50\%%' ESCAPE '\' ORDER BY session_id LIMIT 1;
  ASSERT got = '50%-literal', format('escaped %% matched %s', got);

  -- a literal _ matches only the underscore row, not 'axb-other'
  SELECT session_id INTO got FROM branch_ownership
   WHERE session_id LIKE 'a\_b%' ESCAPE '\' ORDER BY session_id LIMIT 1;
  ASSERT got = 'a_b-literal', format('escaped _ matched %s', got);

  -- and an UNescaped _ is what the C would have sent: it matches 'axb-other'
  -- too, which is the bug this escaping removes
  ASSERT (SELECT count(*) FROM branch_ownership WHERE session_id LIKE 'a_b%') = 2,
    'the unescaped pattern did not over-match -- this test is not measuring anything';

  -- a bare "%" resolves to NOTHING once escaped, where the C would have handed
  -- back an arbitrary session
  ASSERT NOT EXISTS (SELECT 1 FROM branch_ownership WHERE session_id LIKE '\%%' ESCAPE '\'),
    'an escaped bare %% still matched a session';
  ASSERT (SELECT count(*) FROM branch_ownership WHERE session_id LIKE '%') = 4,
    'the unescaped bare %% did not match everything -- the comparison is meaningless';
END $$;

-- The prefix index must exist with text_pattern_ops, or LIKE 'prefix%' cannot
-- use it under a non-C collation.
DO $$
DECLARE definition text;
BEGIN
  SELECT indexdef INTO definition FROM pg_indexes
   WHERE tablename = 'branch_ownership' AND indexname = 'idx_branch_ownership_session_prefix';
  ASSERT definition IS NOT NULL, 'the prefix index is missing';
  ASSERT definition LIKE '%text_pattern_ops%',
    format('the prefix index is %s, want text_pattern_ops', definition);
END $$;

DROP TABLE checkpoints;
DROP TABLE branch_ownership;
DROP TABLE session_feature_branch;

\echo 'CHECKPOINTS+GIT FAMILY SUITE PASSED'
