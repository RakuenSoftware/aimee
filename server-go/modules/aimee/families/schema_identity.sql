-- Native PostgreSQL schema for the remote-client identity grants (kind 11789).
--
-- remote_first_user records who claimed this server. Same singleton fix as the
-- other single-row tables: the SQLite-derived version put an identity sequence
-- on a column constrained to equal 1.
--
-- remote_client_grants pairs a bearer token's digest with a principal, and
-- optionally with the client certificate it was later bound to. The paired
-- CHECK is the invariant that matters: cert_serial and bound_at are set
-- together or not at all, so "enrolled but with no record of when" and "bound
-- at a time but to nothing" are both unrepresentable.

CREATE TABLE IF NOT EXISTS remote_first_user (
  singleton  BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
  principal  TEXT   NOT NULL UNIQUE,
  created_at BIGINT NOT NULL CHECK (created_at >= 0)
);

CREATE TABLE IF NOT EXISTS remote_client_grants (
  bearer_sha256 TEXT PRIMARY KEY CHECK (length(bearer_sha256) = 64 AND bearer_sha256 ~ '^[0-9a-f]*$'),
  principal     TEXT   NOT NULL,
  tier          TEXT   NOT NULL CHECK (tier IN ('data','full')),
  cert_serial   TEXT   UNIQUE CHECK (cert_serial IS NULL OR cert_serial ~ '^[0-9A-Fa-f]{1,79}$'),
  created_at    BIGINT NOT NULL CHECK (created_at >= 0),
  bound_at      BIGINT CHECK (bound_at IS NULL OR bound_at >= created_at),
  CHECK ((cert_serial IS NULL) = (bound_at IS NULL))
);

-- claim looks a principal's grants up directly, preferring a bound one, and
-- tier looks up by certificate serial. The serial already has a unique index
-- from its constraint; this is the one the principal lookup needs.
CREATE INDEX IF NOT EXISTS idx_remote_client_grants_principal
  ON remote_client_grants (principal, created_at);
