-- Native PostgreSQL schema for the sessions family (event kind 11782).
--
-- Five tables that arrived from five sources: the server's own sessions, the
-- per-agent primary transcripts, the write-path log, and the two webchat
-- bindings.
--
-- Every timestamp is TIMESTAMPTZ. The SQLite-derived versions were TEXT holding
-- to_char(now(), ...), which is why the C's expiry queries had to build SQL by
-- string concatenation -- there was no interval arithmetic to do. The wire still
-- carries the same 'YYYY-MM-DD HH24:MI:SS' UTC spelling, formatted at the
-- boundary.

CREATE TABLE IF NOT EXISTS server_sessions (
  id                     TEXT PRIMARY KEY,
  client_type            TEXT NOT NULL DEFAULT '',
  principal              TEXT NOT NULL DEFAULT '',
  title                  TEXT NOT NULL DEFAULT '',
  claude_session_id      TEXT NOT NULL DEFAULT '',
  metadata               TEXT NOT NULL DEFAULT '{}',
  outcome                TEXT NOT NULL DEFAULT '',
  rule_violations        BIGINT NOT NULL DEFAULT 0,
  source                 TEXT NOT NULL DEFAULT '',
  chat_key               TEXT NOT NULL DEFAULT '',
  -- 0 unclaimed, 1 delivered, 2 in flight. The claim is an UPDATE guarded on 0,
  -- so exactly one of two concurrent first requests can take it.
  persona_delivery_state SMALLINT NOT NULL DEFAULT 0
                           CHECK (persona_delivery_state IN (0, 1, 2)),
  created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_activity_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- outcome and title were nullable with a COALESCE at every read site. They are
-- NOT NULL with an empty default instead: a session with no outcome yet and a
-- session whose outcome is the empty string were never distinguishable through
-- this wire, so the nullability bought nothing and cost a COALESCE per query.

-- The recent list and the title search both order by last_activity_at DESC; id
-- breaks the tie so a LIMIT is a stable page.
CREATE INDEX IF NOT EXISTS idx_server_sessions_activity
  ON server_sessions (last_activity_at DESC, id);

-- The expiry sweep and the count-since both filter on created_at.
CREATE INDEX IF NOT EXISTS idx_server_sessions_created
  ON server_sessions (created_at);

CREATE TABLE IF NOT EXISTS primary_sessions (
  session_id    TEXT NOT NULL,
  agent_name    TEXT NOT NULL DEFAULT '',
  provider      TEXT NOT NULL DEFAULT '',
  messages_json TEXT NOT NULL DEFAULT '[]',
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (session_id, agent_name, provider)
);

CREATE INDEX IF NOT EXISTS idx_primary_sessions_updated
  ON primary_sessions (updated_at DESC, session_id);

CREATE TABLE IF NOT EXISTS webchat_claude_sessions (
  principal         TEXT NOT NULL DEFAULT '',
  aimee_session_id  TEXT NOT NULL,
  claude_session_id TEXT NOT NULL DEFAULT '',
  updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (principal, aimee_session_id)
);

-- The ownership check reads by claude_session_id, which the primary key cannot
-- serve: a binding is refused when another tab already owns the id, and that
-- lookup runs on every bind.
CREATE INDEX IF NOT EXISTS idx_webchat_claude_sessions_csid
  ON webchat_claude_sessions (claude_session_id);

-- Reads and updates are by aimee_session_id alone -- principal is attribution,
-- never a namespace -- so it needs an index of its own despite leading the key.
CREATE INDEX IF NOT EXISTS idx_webchat_claude_sessions_aimee
  ON webchat_claude_sessions (aimee_session_id);

CREATE TABLE IF NOT EXISTS webchat_live (
  session_id TEXT PRIMARY KEY,
  turn_id    TEXT   NOT NULL DEFAULT '',
  -- rev advances on every write so a poller can tell the row moved without
  -- diffing the text.
  rev        BIGINT NOT NULL DEFAULT 0,
  text       TEXT   NOT NULL DEFAULT '',
  status     TEXT   NOT NULL DEFAULT 'idle',
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS session_state_write_paths (
  session_id TEXT   NOT NULL,
  seq        BIGINT NOT NULL,
  path       TEXT   NOT NULL,
  PRIMARY KEY (session_id, seq)
);

-- The stale-read query joins read paths to write paths on the path itself, so
-- both sides need an index on it rather than on their session.
CREATE INDEX IF NOT EXISTS idx_session_state_write_paths_path
  ON session_state_write_paths (path);
