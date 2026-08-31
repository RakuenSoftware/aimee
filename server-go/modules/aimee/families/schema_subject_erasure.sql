-- Idempotency journal for the DB1 leg of cross-store subject erasure.
--
-- The raw principal is deliberately absent: only its irreversible digest and
-- the original deletion count survive.  This lets a coordinator retry after
-- DB1 committed but DB2 completion evidence failed without either retaining
-- the erased identity or reporting a misleading zero count.
CREATE TABLE db1_subject_erasure_request (
  request_id       TEXT PRIMARY KEY
                   CHECK (char_length(request_id) BETWEEN 16 AND 128),
  subject_digest   TEXT NOT NULL CHECK (subject_digest ~ '^[0-9a-f]{64}$'),
  deleted_sessions BIGINT NOT NULL CHECK (deleted_sessions >= 0),
  completed_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
