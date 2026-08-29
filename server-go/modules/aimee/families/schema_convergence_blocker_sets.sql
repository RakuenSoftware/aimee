ALTER TABLE wfe_convergence
  ADD COLUMN blocker_set TEXT NOT NULL DEFAULT '';

ALTER TABLE wfe_convergence
  ADD CONSTRAINT wfe_convergence_blocker_set_bounded
  CHECK (octet_length(blocker_set) <= 4160);
