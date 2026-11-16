-- Delivery metadata only. Canonical events remain the sole evidence source.
CREATE TABLE IF NOT EXISTS learning_projection_coverage (
 version INTEGER PRIMARY KEY CHECK (version=1),
 cutoff TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO learning_projection_coverage(version) VALUES(1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS learning_projection_outbox (
 session_id TEXT NOT NULL,
 sequence BIGINT NOT NULL,
 event_id TEXT NOT NULL,
 turn_id TEXT NOT NULL,
 actor TEXT NOT NULL,
 state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','delivered','revoked')),
 attempts INTEGER NOT NULL DEFAULT 0,
 next_attempt TIMESTAMPTZ NOT NULL DEFAULT now(),
 lease_token TEXT NOT NULL DEFAULT '',
 lease_until TIMESTAMPTZ,
 PRIMARY KEY(session_id,sequence),
 FOREIGN KEY(session_id,sequence) REFERENCES session_ir_events(session_id,sequence) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS learning_projection_due ON learning_projection_outbox(next_attempt,session_id,sequence) WHERE state='pending';
CREATE UNIQUE INDEX IF NOT EXISTS learning_projection_lease ON learning_projection_outbox(lease_token) WHERE lease_token<>'';
