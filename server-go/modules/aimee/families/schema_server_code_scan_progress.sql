-- A building scan records when it last made progress, so a second client can
-- tell a scan that is still uploading from one whose client went away. Without
-- it, every begin superseded the scan in flight and concurrent sessions
-- cancelled each other's uploads until none ever sealed.
ALTER TABLE server_code_scans ADD COLUMN IF NOT EXISTS touched_at timestamptz NOT NULL DEFAULT now();
-- Each scan stores every file again, so most versions repeat content already
-- embedded (324k versions held 30k distinct contents on the live Server). A
-- scan reuses a stored embedding for an identical path, content and embedder
-- identity instead of recomputing it; this index serves that lookup.
CREATE INDEX IF NOT EXISTS server_code_file_versions_fingerprint
 ON server_code_file_versions(fingerprint, path) WHERE embedding IS NOT NULL;
