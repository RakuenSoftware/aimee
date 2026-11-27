-- A building scan records when it last made progress, so a second client can
-- tell a scan that is still uploading from one whose client went away. Without
-- it, every begin superseded the scan in flight and concurrent sessions
-- cancelled each other's uploads until none ever sealed.
ALTER TABLE server_code_scans ADD COLUMN IF NOT EXISTS touched_at timestamptz NOT NULL DEFAULT now();
