-- Native PostgreSQL schema for the jti replay stores (event kind 11791).
--
-- These are replay caches for one-shot tokens: a jti may be consumed once, and
-- the primary key is what makes that true. The module validates before it
-- writes so a bad record is refused with INVALID rather than a constraint
-- error, but the CHECKs stay -- validation in the module says what this process
-- accepts, and a constraint says what the store will hold no matter who writes.
--
-- issued_at / expires_at / consumed_at stay BIGINT rather than becoming
-- TIMESTAMPTZ, which is a deliberate exception to preferring the native type.
-- They are token epochs supplied by the caller and compared against each other,
-- never against the database's clock; converting them would introduce a
-- timezone and a rounding question into a comparison that today is exact.

CREATE TABLE IF NOT EXISTS server_identity_jti (
  jti         TEXT PRIMARY KEY CHECK (length(jti) BETWEEN 8 AND 128 AND jti ~ '^[A-Za-z0-9._-]*$'),
  issuer      TEXT   NOT NULL CHECK (length(issuer) BETWEEN 1 AND 255 AND issuer !~ '[\x01-\x1F\x7F]'),
  kid         TEXT   NOT NULL CHECK (length(kid) BETWEEN 1 AND 64 AND kid ~ '^[A-Za-z0-9._-]*$'),
  audience    TEXT   NOT NULL CHECK (length(audience) BETWEEN 1 AND 127 AND audience ~ '^[A-Za-z0-9._-]*$'),
  subject     TEXT   NOT NULL CHECK (length(subject) BETWEEN 1 AND 576 AND subject !~ '[\x01-\x1F\x7F]'),
  team_id     BIGINT NOT NULL CHECK (team_id > 0),
  tier        TEXT   NOT NULL CHECK (tier IN ('off','data','full')),
  issued_at   BIGINT NOT NULL CHECK (issued_at >= 0),
  expires_at  BIGINT NOT NULL CHECK (expires_at > issued_at),
  consumed_at BIGINT NOT NULL CHECK (consumed_at >= issued_at AND consumed_at < expires_at)
);

-- The garbage-collection pass reads (expires_at, jti) in that order and deletes
-- a bounded batch, so this index is what keeps the sweep from scanning the
-- whole table on every consume.
CREATE INDEX IF NOT EXISTS idx_server_identity_jti_expiry
  ON server_identity_jti (expires_at, jti);

CREATE TABLE IF NOT EXISTS server_management_jti (
  jti              TEXT PRIMARY KEY CHECK (length(jti) BETWEEN 16 AND 128 AND jti ~ '^[A-Za-z0-9._-]*$'),
  issuer           TEXT   NOT NULL CHECK (length(issuer) BETWEEN 1 AND 255 AND issuer !~ '[\x01-\x1F\x7F]'),
  kid              TEXT   NOT NULL CHECK (length(kid) BETWEEN 1 AND 64 AND kid ~ '^[A-Za-z0-9._-]*$'),
  audience         TEXT   NOT NULL CHECK (length(audience) BETWEEN 1 AND 127 AND audience ~ '^[A-Za-z0-9._-]*$'),
  subject          TEXT   NOT NULL CHECK (length(subject) BETWEEN 1 AND 576 AND subject !~ '[\x01-\x1F\x7F]'),
  team_id          BIGINT NOT NULL CHECK (team_id > 0),
  capability       TEXT   NOT NULL CHECK (length(capability) BETWEEN 1 AND 64 AND capability ~ '^[A-Za-z0-9._-]*$'),
  peer_issuer      TEXT   NOT NULL CHECK (length(peer_issuer) BETWEEN 1 AND 511 AND peer_issuer !~ '[\x01-\x1F\x7F]'),
  peer_serial      TEXT   NOT NULL CHECK (length(peer_serial) BETWEEN 1 AND 79 AND peer_serial ~ '^[0-9a-f]*$'),
  peer_fingerprint TEXT   NOT NULL CHECK (length(peer_fingerprint) = 64 AND peer_fingerprint ~ '^[0-9a-f]*$'),
  request_sha256   TEXT   NOT NULL CHECK (length(request_sha256) = 64 AND request_sha256 ~ '^[0-9a-f]*$'),
  correlation_id   TEXT   NOT NULL CHECK (length(correlation_id) BETWEEN 1 AND 128 AND correlation_id ~ '^[A-Za-z0-9._-]*$'),
  issued_at        BIGINT NOT NULL CHECK (issued_at >= 0),
  expires_at       BIGINT NOT NULL CHECK (expires_at > issued_at),
  consumed_at      BIGINT NOT NULL CHECK (consumed_at >= issued_at AND consumed_at < expires_at)
);

CREATE INDEX IF NOT EXISTS idx_server_management_jti_expiry
  ON server_management_jti (expires_at, jti);
