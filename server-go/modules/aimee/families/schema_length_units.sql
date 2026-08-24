-- Count the same unit the consumers count.
--
-- THE DEFECT. Six TEXT columns are checked with length(), which counts
-- CHARACTERS, while every consumer of those values counts BYTES -- and both use
-- the same number, so the disagreement is invisible on the page:
--
--   server_identity_jti.subject     CHECK (length(subject) <= 576)
--   src/server/server_mgmt_token.h  char subject[577];   -- 576 BYTES + NUL
--
-- The regex on these columns is `!~ '[\x01-\x1F\x7F]'`, which excludes ASCII
-- control characters and permits everything else, multi-byte included. So a
-- subject of 300 two-byte characters is 300 characters and 600 bytes: accepted
-- by the database, too large for the C buffer that reads it back. Written,
-- then truncated or refused on the way out, with nothing at write time saying
-- so. It is the same shape as migration 20, arrived at from the other side --
-- there the column had no limit at all, here it has one that counts wrong.
--
-- Found because the postgres module hit the identical defect on db2's
-- kb_write_tier_grant.subject, also at 576, also from a C buffer of
-- char subject[577]. Same number, three units: the database counting
-- characters, the C buffer holding bytes, the wire declaring bytes.
--
-- WHY octet_length IS THE RIGHT UNIT rather than a preference: the C buffer
-- physically cannot hold more than 576 bytes. The database was permitting
-- values its own reader could never return. Aligning on bytes makes the
-- constraint mean what every consumer already assumed it meant.
--
-- ASCII-ONLY COLUMNS ARE LEFT ALONE. jti, kid, audience, capability,
-- peer_serial, peer_fingerprint, request_sha256, correlation_id and the hash
-- columns all carry a regex restricting them to ASCII, where length() and
-- octet_length() cannot disagree. Changing them would be churn that implies a
-- defect where there is none. BYTEA columns are also untouched: length() on
-- bytea already counts bytes.
--
-- The constraint bodies are restated in full rather than patched, because a
-- CHECK is one expression -- the length test and the regex are a single
-- constraint, so replacing the unit means rewriting the whole predicate. The
-- regexes below are copied unchanged from migrations 10 and 14; only length(
-- becomes octet_length(.
--
-- DROP ... IF EXISTS, then ADD: if PostgreSQL ever named one of these
-- constraints differently and the drop finds nothing, the ADD still applies and
-- the byte limit -- being the stricter of the two -- is the one that binds. The
-- migration is safe under that uncertainty rather than dependent on it.

ALTER TABLE server_identity_jti
  DROP CONSTRAINT IF EXISTS server_identity_jti_issuer_check;
ALTER TABLE server_identity_jti
  ADD CONSTRAINT server_identity_jti_issuer_check
  CHECK (octet_length(issuer) BETWEEN 1 AND 255 AND issuer !~ '[\x01-\x1F\x7F]');

ALTER TABLE server_identity_jti
  DROP CONSTRAINT IF EXISTS server_identity_jti_subject_check;
ALTER TABLE server_identity_jti
  ADD CONSTRAINT server_identity_jti_subject_check
  CHECK (octet_length(subject) BETWEEN 1 AND 576 AND subject !~ '[\x01-\x1F\x7F]');

ALTER TABLE server_management_jti
  DROP CONSTRAINT IF EXISTS server_management_jti_issuer_check;
ALTER TABLE server_management_jti
  ADD CONSTRAINT server_management_jti_issuer_check
  CHECK (octet_length(issuer) BETWEEN 1 AND 255 AND issuer !~ '[\x01-\x1F\x7F]');

ALTER TABLE server_management_jti
  DROP CONSTRAINT IF EXISTS server_management_jti_subject_check;
ALTER TABLE server_management_jti
  ADD CONSTRAINT server_management_jti_subject_check
  CHECK (octet_length(subject) BETWEEN 1 AND 576 AND subject !~ '[\x01-\x1F\x7F]');

ALTER TABLE server_management_jti
  DROP CONSTRAINT IF EXISTS server_management_jti_peer_issuer_check;
ALTER TABLE server_management_jti
  ADD CONSTRAINT server_management_jti_peer_issuer_check
  CHECK (octet_length(peer_issuer) BETWEEN 1 AND 511 AND peer_issuer !~ '[\x01-\x1F\x7F]');

-- pki_certs.cn has no regex at all, so it permits multi-byte outright. Its
-- reader in src/server/pki.c is char cn[256], i.e. 255 bytes.
--
-- NOTE, NOT FIXED HERE: src/modules/vault/vault_principal.h sets
-- VAULT_CERT_CN_MAX to 128, so two consumers of this column disagree about its
-- size in bytes as well as its unit. That is a separate question from which
-- unit to count, it needs whoever owns the vault boundary to say which is
-- right, and quietly picking one here would bury it.
ALTER TABLE pki_certs
  DROP CONSTRAINT IF EXISTS pki_certs_cn_check;
ALTER TABLE pki_certs
  ADD CONSTRAINT pki_certs_cn_check
  CHECK (octet_length(cn) <= 255);

-- pki_certs.serial has no regex either. I nearly left this one out, on the
-- grounds that a certificate serial is hex in practice -- but nothing in the
-- schema says so, and "in practice" is not a constraint. The unit is wrong for
-- the same reason as the rest, so it is fixed for the same reason.
--
-- NOTE, NOT FIXED HERE, and a second instance of the pattern above: the
-- database permits 127 while its readers are char serial[80] in
-- src/server/server_http_identity.c and char serial[64] in src/server/pki.c.
-- Those are size disagreements, not unit disagreements -- a plain ASCII serial
-- of 100 characters is within the column and past both buffers. Narrowing the
-- column to match would be a semantic change to what a serial may be, which is
-- not this migration's business and not mine to decide quietly.
ALTER TABLE pki_certs
  DROP CONSTRAINT IF EXISTS pki_certs_serial_check;
ALTER TABLE pki_certs
  ADD CONSTRAINT pki_certs_serial_check
  CHECK (octet_length(serial) BETWEEN 1 AND 127);
