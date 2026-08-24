-- Native PostgreSQL schema for guardrail session state (event kind 11785).
--
-- One parent row per session plus six child tables. The child tables are
-- rewritten wholesale on every save -- the state they hold is a snapshot, not a
-- log -- so ON DELETE CASCADE is what keeps a deleted session from leaving
-- orphans behind.
--
-- content_hash is BIGINT holding a UINT64 by two's-complement reinterpretation.
-- It is an FNV-1a hash: opaque, compared only for equality, never summed,
-- ordered or ranged. The values run to 2^64-1, which BIGINT cannot represent as
-- a magnitude, so the module converts the bits on the way in and back on the
-- way out and the round trip is exact. The SQLite-derived schema made this
-- column TEXT, which sidesteps the range question by storing a number as a
-- string; NUMERIC(20,0) would hold the magnitude but buys nothing for a value
-- nothing ever does arithmetic on.
--
-- created_at/updated_at are TIMESTAMPTZ. The wire still carries the
-- 'YYYY-MM-DD HH24:MI:SS' UTC spelling, formatted at the boundary.

CREATE TABLE IF NOT EXISTS session_state (
  session_id                            TEXT PRIMARY KEY,
  session_mode                          TEXT   NOT NULL DEFAULT 'implement',
  guardrail_mode                        TEXT   NOT NULL DEFAULT 'approve',
  tdd_mode                              TEXT   NOT NULL DEFAULT 'off',
  active_task_id                        BIGINT NOT NULL DEFAULT 0,
  hook_call_count                       BIGINT NOT NULL DEFAULT 0,
  orch_direct_edits                     BIGINT NOT NULL DEFAULT 0,
  orch_nudge_sent                       BIGINT NOT NULL DEFAULT 0,
  skill_find_symbols_advisory_sent      BIGINT NOT NULL DEFAULT 0,
  skill_condition_waiting_advisory_sent BIGINT NOT NULL DEFAULT 0,
  skill_tdd_advisory_sent               BIGINT NOT NULL DEFAULT 0,
  created_at                            TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at                            TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- The expiry sweep reads sessions whose updated_at is older than a threshold.
CREATE INDEX IF NOT EXISTS idx_session_state_updated
  ON session_state (updated_at);

CREATE TABLE IF NOT EXISTS session_state_seen_paths (
  session_id TEXT   NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE,
  seq        BIGINT NOT NULL,
  path       TEXT   NOT NULL,
  PRIMARY KEY (session_id, seq)
);

CREATE TABLE IF NOT EXISTS session_state_read_paths (
  session_id TEXT   NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE,
  seq        BIGINT NOT NULL,
  path       TEXT   NOT NULL,
  PRIMARY KEY (session_id, seq)
);

CREATE TABLE IF NOT EXISTS session_state_worktrees (
  session_id    TEXT   NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE,
  seq           BIGINT NOT NULL,
  git_root      TEXT   NOT NULL,
  worktree_path TEXT   NOT NULL,
  PRIMARY KEY (session_id, seq)
);

CREATE TABLE IF NOT EXISTS session_state_tdd_writes (
  session_id TEXT    NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE,
  seq        BIGINT  NOT NULL,
  stem       TEXT    NOT NULL,
  is_test    BOOLEAN NOT NULL DEFAULT false,
  PRIMARY KEY (session_id, seq)
);

CREATE TABLE IF NOT EXISTS session_state_ap_hits (
  session_id TEXT   NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE,
  pattern_id BIGINT NOT NULL,
  hits       BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (session_id, pattern_id)
);

CREATE TABLE IF NOT EXISTS session_state_file_hashes (
  session_id   TEXT   NOT NULL REFERENCES session_state(session_id) ON DELETE CASCADE,
  path         TEXT   NOT NULL,
  content_hash BIGINT NOT NULL,
  PRIMARY KEY (session_id, path)
);
