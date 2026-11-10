-- Extend the existing opaque cache without changing the recorded runtime
-- migration. Existing entries retain their value and begin at revision one.
ALTER TABLE context_cache ADD COLUMN IF NOT EXISTS revision BIGINT NOT NULL DEFAULT 1;

CREATE TABLE IF NOT EXISTS session_cache_receipts (
 hash TEXT NOT NULL REFERENCES context_cache(hash) ON DELETE CASCADE,
 session_id TEXT NOT NULL, nonce TEXT NOT NULL,
 expected_revision BIGINT NOT NULL, revision BIGINT NOT NULL, output TEXT NOT NULL,
 PRIMARY KEY(hash,session_id,nonce)
);
