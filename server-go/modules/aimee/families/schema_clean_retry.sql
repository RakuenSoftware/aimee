-- Retry reservations survive disposable task/session projections.
ALTER TABLE governed_action_roots ADD COLUMN retry_journal TEXT NOT NULL DEFAULT '' CHECK(octet_length(retry_journal)<=4194304);
