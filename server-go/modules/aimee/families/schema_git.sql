-- Native PostgreSQL schema for git branch ownership (event kind 11778).
--
-- Two tables: which session owns a branch in a repo, and which feature branch a
-- session is working on there.
--
-- The surrogate id columns are gone. Both tables already carried a UNIQUE on
-- the pair that identifies a row, and both are only ever read and written by
-- that pair -- nothing selects, joins or deletes by id. A separate identity
-- column plus a unique constraint on the real key is two keys where the store
-- needs one, so the real key is the primary key.
--
-- created_at becomes TIMESTAMPTZ for the same reason as everywhere else: the
-- transliterated column was TEXT holding to_char(now(), ...).

CREATE TABLE IF NOT EXISTS branch_ownership (
  repo_path   TEXT NOT NULL,
  branch_name TEXT NOT NULL,
  session_id  TEXT NOT NULL,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (repo_path, branch_name)
);

-- "which branch does this session own here" reads by (repo_path, session_id),
-- which the primary key cannot serve.
CREATE INDEX IF NOT EXISTS idx_branch_ownership_session
  ON branch_ownership (repo_path, session_id);

-- The prefix lookup scans session_id with a literal prefix pattern. text_pattern_ops
-- is what lets a LIKE 'prefix%' use this index regardless of the database's
-- collation -- under a non-C collation a default index cannot serve it.
CREATE INDEX IF NOT EXISTS idx_branch_ownership_session_prefix
  ON branch_ownership (session_id text_pattern_ops);

CREATE TABLE IF NOT EXISTS session_feature_branch (
  repo_path      TEXT NOT NULL,
  session_id     TEXT NOT NULL,
  feature_branch TEXT NOT NULL,
  created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (repo_path, session_id)
);
