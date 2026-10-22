-- Migration 49: pre-exec source custody and a shared physical cache epoch.
CREATE TABLE IF NOT EXISTS session_native_cache_epochs (
 session_id TEXT NOT NULL REFERENCES server_sessions(id) ON DELETE CASCADE,
 harness TEXT NOT NULL CHECK(harness IN ('codex','claude')),
 binding_sha256 TEXT NOT NULL,
 actor TEXT NOT NULL,
 epoch TEXT NOT NULL DEFAULT '0',
 PRIMARY KEY(session_id,harness,binding_sha256)
);
CREATE TABLE IF NOT EXISTS session_native_reservations (
 session_id TEXT NOT NULL REFERENCES server_sessions(id) ON DELETE CASCADE,
 harness TEXT NOT NULL CHECK(harness IN ('codex','claude')),
 binding_sha256 TEXT NOT NULL,
 owner_id TEXT NOT NULL,
 turn_id TEXT NOT NULL,
 actor TEXT NOT NULL,
 epoch TEXT NOT NULL,
 containment_sha256 TEXT NOT NULL,
 containment_json TEXT NOT NULL CHECK(octet_length(containment_json)<=8192),
 claim_id TEXT NOT NULL DEFAULT '',
 scope_sha256 TEXT NOT NULL DEFAULT '',
 context_sha256 TEXT NOT NULL DEFAULT '',
 state TEXT NOT NULL DEFAULT 'active' CHECK(state IN ('active','reconciled','finished')),
 terminal_sequence BIGINT,
 recovery_epoch TEXT NOT NULL DEFAULT '',
 recovered_from TEXT NOT NULL DEFAULT '',
 finished_from TEXT NOT NULL DEFAULT '',
 created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 PRIMARY KEY(session_id,harness,binding_sha256,owner_id)
);
CREATE UNIQUE INDEX IF NOT EXISTS session_native_reservation_active
 ON session_native_reservations(session_id,harness,binding_sha256) WHERE state='active';
ALTER TABLE session_native_cache_owners ADD COLUMN IF NOT EXISTS cache_epoch TEXT NOT NULL DEFAULT '';
