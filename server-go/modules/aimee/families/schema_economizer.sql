-- Native PostgreSQL schema for the DB1 state family (event kind 11777).
--
-- The C module stored this in `checkpoints` under a magic label
-- ('economizer-state'), which is two unrelated things in one table: checkpoints
-- are rows, this is ONE opaque document per session. checkpoints.c was already
-- split in the source for exactly that reason; the table was never split with
-- it. This finishes the split.
--
-- Three things here are native rather than transliterated from SQLite:
--
--   session_id as PRIMARY KEY. The C kept "exactly one row" by DELETE-then-
--   INSERT, which is a convention the database does not enforce and which has a
--   window where the row is absent. A primary key makes it an invariant, and
--   the write becomes one atomic upsert with no window and no id churn.
--
--   updated_at as TIMESTAMPTZ DEFAULT now(). checkpoints.created_at is
--   TEXT DEFAULT to_char(now() AT TIME ZONE 'utc', ...) -- a string that sorts
--   correctly only by accident of its format, cannot be compared against an
--   interval, and silently drops the zone.
--
--   state stays TEXT, deliberately, even though the payload is JSON. The module
--   treats it as opaque -- it neither reads nor indexes inside it -- and JSONB
--   would make the module enforce a schema it does not own, turning a blob the
--   C module accepted into a refusal. JSONB is the right upgrade the moment
--   something needs to query inside the document, and not before.
CREATE TABLE IF NOT EXISTS economizer_state (
  session_id TEXT PRIMARY KEY,
  state      TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
