-- Preserve every existing first-user certificate and grant.
ALTER TABLE remote_client_grants
  ADD COLUMN device_name TEXT NOT NULL DEFAULT 'First client' CHECK (octet_length(device_name) BETWEEN 1 AND 64),
  ADD COLUMN expires_at BIGINT NOT NULL DEFAULT 0 CHECK (expires_at >= 0),
  ADD COLUMN revoked_at BIGINT CHECK (revoked_at IS NULL OR revoked_at >= created_at);
