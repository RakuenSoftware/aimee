-- Durable action state is not disposable prompt/session state. Deleting a
-- session or rolling back a retry does not remove receipts or spent allowance.
CREATE TABLE governed_action_roots (
 principal TEXT NOT NULL,
 root_id TEXT NOT NULL,
 journal TEXT NOT NULL DEFAULT '' CHECK(octet_length(journal)<=4194304),
 created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 PRIMARY KEY(principal,root_id)
);
CREATE TABLE governed_action_sessions (
 principal TEXT NOT NULL,
 session_id TEXT NOT NULL,
 root_id TEXT NOT NULL,
 created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 PRIMARY KEY(principal,session_id),
 FOREIGN KEY(principal,root_id) REFERENCES governed_action_roots(principal,root_id)
);
