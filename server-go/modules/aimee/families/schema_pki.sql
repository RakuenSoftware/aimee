-- Native PostgreSQL schema for the PKI roster and mTLS ramp (kind 11795).
--
-- pki_certs is this server's certificate roster. pki_mtls_ramp is a single row
-- holding how far mTLS enforcement has been rolled out, plus the hash of the
-- roster that state was reached with.
--
-- revoked becomes BOOLEAN. It was a BIGINT holding 0 or 1, which is an integer
-- standing in for a fact that has two values; the wire still carries 0/1 and
-- the module converts at the boundary.
--
-- The ramp's id column is gone, replaced by the same boolean-singleton the
-- other one-row tables use.

CREATE TABLE IF NOT EXISTS pki_certs (
  serial            TEXT PRIMARY KEY CHECK (length(serial) BETWEEN 1 AND 127),
  cn                TEXT    NOT NULL CHECK (length(cn) <= 255),
  issued_at         BIGINT  NOT NULL DEFAULT 0,
  expires_at        BIGINT  NOT NULL DEFAULT 0,
  revoked           BOOLEAN NOT NULL DEFAULT false,
  last_presented_at BIGINT  NOT NULL DEFAULT 0
);

-- The list reads newest-first; serial breaks the tie so a LIMIT is a stable
-- page rather than an arbitrary one among certificates issued in the same
-- second.
CREATE INDEX IF NOT EXISTS idx_pki_certs_issued
  ON pki_certs (issued_at DESC, serial);

-- The revocation list and the roster snapshot both filter on revoked.
CREATE INDEX IF NOT EXISTS idx_pki_certs_revoked
  ON pki_certs (revoked) WHERE revoked;

CREATE TABLE IF NOT EXISTS pki_mtls_ramp (
  singleton       BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
  ramp_state      SMALLINT NOT NULL CHECK (ramp_state IN (1, 2)),
  roster_hash     TEXT     NOT NULL CHECK (length(roster_hash) = 64 AND roster_hash ~ '^[0-9a-f]*$'),
  last_advance_ts BIGINT   NOT NULL DEFAULT 0
);
