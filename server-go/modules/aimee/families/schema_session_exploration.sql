-- Host-owned exploration state follows the existing session row's lifetime.
-- Legacy session save operations intentionally do not update these columns.
ALTER TABLE session_state
 ADD COLUMN exploration_state TEXT NOT NULL DEFAULT ''
 CHECK (octet_length(exploration_state) <= 1048576);
