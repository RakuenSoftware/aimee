-- Append-only migration 45. Native resume handles are host runtime metadata,
-- separate from canonical conversation content and namespaced by harness.
CREATE TABLE IF NOT EXISTS session_harness_handles (
  session_id TEXT NOT NULL REFERENCES server_sessions(id) ON DELETE CASCADE,
  harness TEXT NOT NULL,
  native_id TEXT NOT NULL,
  PRIMARY KEY (session_id, harness),
  UNIQUE (harness, native_id)
);
