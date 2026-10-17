-- Append-only migration 44: canonical typed session log.
-- Canonical session content, owned by the same lifecycle as server_sessions.
-- Provider resume state is separate from transcript identity. Deleting/erasing
-- a session cascades to every event and its sequence allocator.
CREATE TABLE IF NOT EXISTS session_ir_events (
  session_id TEXT NOT NULL REFERENCES server_sessions(id) ON DELETE CASCADE,
  sequence BIGINT NOT NULL CHECK (sequence > 0),
  event_id TEXT NOT NULL,
  turn_id TEXT NOT NULL,
  actor TEXT NOT NULL,
  payload TEXT NOT NULL CHECK (octet_length(payload) <= 1048576),
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (session_id, sequence),
  UNIQUE (session_id, event_id)
);
