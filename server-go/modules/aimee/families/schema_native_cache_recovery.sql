-- Migration 48: host-only native-cache custody and recovered-source write fence.
-- These rows are resource-owner metadata, never execution/test evidence.
CREATE TABLE IF NOT EXISTS session_native_cache_owners (
 session_id TEXT NOT NULL REFERENCES server_sessions(id) ON DELETE CASCADE,
 cache_key TEXT NOT NULL,
 actor TEXT NOT NULL,
 turn_id TEXT NOT NULL,
 harness TEXT NOT NULL CHECK(harness IN ('codex','claude')),
 scope_sha256 TEXT NOT NULL,
 context_sha256 TEXT NOT NULL,
 binding_sha256 TEXT NOT NULL,
 owner_id TEXT NOT NULL,
 claim_id TEXT NOT NULL,
 containment_sha256 TEXT NOT NULL,
 recovered_from TEXT NOT NULL DEFAULT '',
 terminal_sequence BIGINT,
 PRIMARY KEY(session_id,cache_key)
);
CREATE TABLE IF NOT EXISTS session_native_recovery_fences (
 session_id TEXT NOT NULL,
 turn_id TEXT NOT NULL,
 actor TEXT NOT NULL,
 owner_id TEXT NOT NULL,
 claim_id TEXT NOT NULL,
 containment_sha256 TEXT NOT NULL,
 terminal_sequence BIGINT NOT NULL CHECK(terminal_sequence>0),
 PRIMARY KEY(session_id,turn_id),
 FOREIGN KEY(session_id) REFERENCES server_sessions(id) ON DELETE CASCADE
);
