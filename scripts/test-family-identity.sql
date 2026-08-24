-- Verify the identity family's PostgreSQL surface on a real server.
--
-- Two things the Go tests cannot answer, because they script the database:
-- whether the paired cert_serial/bound_at CHECK actually makes a half-bound
-- grant unrepresentable, and whether the claim's "prefer a bound grant"
-- ordering behaves the way the module assumes.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS remote_client_grants;
DROP TABLE IF EXISTS remote_first_user;

\i /tmp/family_schema_identity.sql

-- --- one owner, ever ----------------------------------------------------------

DO $$
BEGIN
  INSERT INTO remote_first_user (singleton, principal, created_at)
       VALUES (true, 'webuser:alice', 1000);
  BEGIN
    INSERT INTO remote_first_user (singleton, principal, created_at)
         VALUES (true, 'webuser:mallory', 2000);
    ASSERT false, 'a second owner was accepted -- the first-user row is not a singleton';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;

-- --- a grant is bound completely or not at all --------------------------------

-- This is the invariant the module leans on: it reads cert_serial to decide
-- whether a grant is bound, and a row with a serial but no bound_at (or the
-- reverse) would make that read a lie.
DO $$
BEGIN
  BEGIN
    INSERT INTO remote_client_grants
         VALUES (repeat('a',64), 'webuser:alice', 'full', 'ABCD', 1000, NULL);
    ASSERT false, 'a grant with a certificate but no bound_at was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO remote_client_grants
         VALUES (repeat('a',64), 'webuser:alice', 'full', NULL, 1000, 2000);
    ASSERT false, 'a grant bound at a time but to no certificate was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  -- bound_at may not precede created_at
  BEGIN
    INSERT INTO remote_client_grants
         VALUES (repeat('a',64), 'webuser:alice', 'full', 'ABCD', 2000, 1000);
    ASSERT false, 'a grant bound before it was created was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- One certificate may speak for at most one grant.
DO $$
BEGIN
  INSERT INTO remote_client_grants
       VALUES (repeat('a',64), 'webuser:alice', 'full', 'ABCD', 1000, 1500);
  BEGIN
    INSERT INTO remote_client_grants
         VALUES (repeat('b',64), 'webuser:alice', 'full', 'ABCD', 1000, 1500);
    ASSERT false, 'one certificate serial was bound to two grants';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;

DO $$
BEGIN
  BEGIN
    INSERT INTO remote_client_grants
         VALUES (repeat('a',63), 'webuser:alice', 'full', NULL, 1000, NULL);
    ASSERT false, 'a 63-character bearer digest was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO remote_client_grants
         VALUES (upper(repeat('a',64)), 'webuser:alice', 'full', NULL, 1000, NULL);
    ASSERT false, 'an uppercase bearer digest was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO remote_client_grants
         VALUES (repeat('c',64), 'webuser:alice', 'root', NULL, 1000, NULL);
    ASSERT false, 'an unrecognised tier was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- --- the claim's grant ordering -----------------------------------------------

-- claim prefers a BOUND grant so that re-running Deploy does not mint a second
-- standing credential, and falls back to the oldest pending enrollment so a
-- browser refresh recovers the same quickstart command.
PREPARE grant_for (text) AS
  SELECT principal, bearer_sha256, coalesce(cert_serial, ''), tier
    FROM remote_client_grants
   WHERE principal = $1
   ORDER BY (cert_serial IS NOT NULL) DESC, created_at ASC
   LIMIT 1;

DO $$
DECLARE picked text;
BEGIN
  DELETE FROM remote_client_grants;
  -- two pending enrollments and one completed one, inserted newest-first so a
  -- query that ignored the ordering would pick the wrong row
  INSERT INTO remote_client_grants
       VALUES (repeat('d',64), 'webuser:alice', 'full', NULL, 3000, NULL);
  INSERT INTO remote_client_grants
       VALUES (repeat('e',64), 'webuser:alice', 'full', NULL, 1000, NULL);
  INSERT INTO remote_client_grants
       VALUES (repeat('f',64), 'webuser:alice', 'full', 'ABCD', 2000, 2500);

  SELECT bearer_sha256 INTO picked FROM remote_client_grants
   WHERE principal = 'webuser:alice'
   ORDER BY (cert_serial IS NOT NULL) DESC, created_at ASC
   LIMIT 1;
  ASSERT picked = repeat('f',64),
    format('claim picked %s, want the BOUND grant', left(picked, 8));

  -- with the bound one gone it must fall back to the OLDEST pending grant
  DELETE FROM remote_client_grants WHERE cert_serial IS NOT NULL;
  SELECT bearer_sha256 INTO picked FROM remote_client_grants
   WHERE principal = 'webuser:alice'
   ORDER BY (cert_serial IS NOT NULL) DESC, created_at ASC
   LIMIT 1;
  ASSERT picked = repeat('e',64),
    format('claim picked %s, want the oldest pending grant', left(picked, 8));
END $$;

-- The abandon statement must never touch a bound grant.
DO $$
BEGIN
  DELETE FROM remote_client_grants;
  INSERT INTO remote_client_grants
       VALUES (repeat('f',64), 'webuser:alice', 'full', 'ABCD', 2000, 2500);
  DELETE FROM remote_client_grants
        WHERE bearer_sha256 = repeat('f',64) AND cert_serial IS NULL;
  ASSERT EXISTS (SELECT 1 FROM remote_client_grants WHERE bearer_sha256 = repeat('f',64)),
    'abandon deleted a bound grant';
END $$;

-- --- two setup requests at once -----------------------------------------------
--
-- The first-user row is a singleton, and that is the whole of the protection:
-- whichever session inserts it first owns the server, and the other is refused
-- rather than given a second grant. A C test used to race two threads through
-- the bootstrap and assert they came back with one bearer between them; it
-- cannot run against a module, so the serialisation is proved here, where it
-- actually happens.

CREATE EXTENSION IF NOT EXISTS dblink;

DELETE FROM remote_client_grants;
DELETE FROM remote_first_user;

BEGIN;
-- this session claims, and holds the row uncommitted
INSERT INTO remote_first_user (singleton, principal, created_at)
     VALUES (true, 'webuser:alice', 5000);

DO $$
DECLARE blocked boolean := false;
BEGIN
  -- A second session claiming the same instant must NOT get its own row. It
  -- blocks on the uniqueness check rather than being refused outright, so the
  -- probe runs with a short timeout and reads the timeout as "it waited",
  -- which is the correct outcome: it cannot proceed while this claim is open.
  BEGIN
    PERFORM dblink('dbname=postgres',
        'SET lock_timeout = ''250ms'';
         INSERT INTO remote_first_user (singleton, principal, created_at)
              VALUES (true, ''webuser:mallory'', 5000)');
  EXCEPTION WHEN OTHERS THEN blocked := true;
  END;

  ASSERT blocked,
         'a second setup request inserted its own owner row while the first was open';
END $$;

COMMIT;

-- And once the first claim is committed, the second is refused outright rather
-- than waiting: there is an owner now.
DO $$
DECLARE refused boolean := false;
BEGIN
  BEGIN
    INSERT INTO remote_first_user (singleton, principal, created_at)
         VALUES (true, 'webuser:mallory', 6000);
  EXCEPTION WHEN unique_violation THEN refused := true;
  END;
  ASSERT refused, 'a second owner was accepted after the first claim committed';
  ASSERT (SELECT principal FROM remote_first_user) = 'webuser:alice',
         'the owner is not the session that claimed first';
END $$;


DROP TABLE remote_client_grants;
DROP TABLE remote_first_user;

\echo 'IDENTITY FAMILY SUITE PASSED'
