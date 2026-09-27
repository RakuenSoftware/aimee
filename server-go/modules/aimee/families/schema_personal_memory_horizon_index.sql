-- Read-only anchor lookup for the Go memory utility horizon owner. No new
-- content history or write permission is introduced.
CREATE INDEX user_memory_invalidation_anchor
 ON user_memory_invalidation_outbox(memory_id,operation,record_revision,generation)
 INCLUDE(recorded_at);
