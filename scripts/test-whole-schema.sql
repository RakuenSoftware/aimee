-- Apply the WHOLE store schema to one database and check it agrees with itself.
--
-- The per-family suites each apply only their own family's schema file. That is
-- the right isolation for testing a family's behaviour, and it is exactly why
-- none of them could see that agent_log was declared in TWO files with two
-- different types for `success` -- each suite met only its own definition and
-- passed. Whichever file applied first won in silence, because both said
-- IF NOT EXISTS, and which one that was depended on file order.
--
-- A real deployment applies all of them to one database, so this is the shape
-- that matters and the only place a conflict between two families is visible.
--
-- Run AFTER every schema file has been applied to this database in one pass.

\set ON_ERROR_STOP on

-- --- every table the module needs actually exists ------------------------------

DO $$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n
    FROM information_schema.tables
   WHERE table_schema = current_schema() AND table_type = 'BASE TABLE';
  ASSERT n > 80, format('only %s tables after applying every schema file', n);
END $$;

-- --- no column carries two different types -------------------------------------
--
-- This is the direct check for the failure above. A table declared twice with
-- IF NOT EXISTS silently keeps the first shape, so the way to catch it is to
-- compare what was CREATED against what the files SAY -- which the harness does
-- by applying everything and then asking the catalog. Here we assert the
-- properties the module's own SQL depends on, one row per column that a
-- disagreement would break.

DO $$
DECLARE got text;
BEGIN
  -- agent_work writes `WHERE NOT success` and `FILTER (WHERE success)`, which
  -- only work on a real boolean. delegation reads the same column and used to
  -- declare it BIGINT.
  SELECT data_type INTO got
    FROM information_schema.columns
   WHERE table_schema = current_schema()
     AND table_name = 'agent_log' AND column_name = 'success';
  ASSERT got = 'boolean',
    format('agent_log.success is %s, want boolean -- two schema files disagree', got);
END $$;

-- --- the confidence sentinel survives ------------------------------------------
--
-- -1 means "not scored" and 0 is a real, very different answer. One of the two
-- agent_log definitions constrained this to -1..100 and the other did not; the
-- constraint is the one worth keeping, so prove it is there.
DO $$
DECLARE rejected boolean := false;
BEGIN
  BEGIN
    INSERT INTO agent_log (role, confidence) VALUES ('probe', 101);
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected, 'agent_log.confidence accepted 101 -- the range check is missing';

  DELETE FROM agent_log WHERE role = 'probe';
  INSERT INTO agent_log (role, confidence) VALUES ('probe', -1);
  ASSERT (SELECT confidence FROM agent_log WHERE role = 'probe') = -1,
    'agent_log.confidence rejected the -1 "not scored" sentinel';
  DELETE FROM agent_log WHERE role = 'probe';
END $$;

-- --- a value the wire cannot return is refused on the way in -------------------
--
-- The store wire caps a cell at 1 MiB and REFUSES an over-large one rather than
-- truncating it. Twelve columns feeding 1 MiB catalog fields were unbounded
-- TEXT, so a caller could write a value that every later read would fail on:
-- the row not corrupted, just permanently unreachable, with nothing at write
-- time saying so. Migration 20 caps them.
--
-- Checked here rather than in a per-family suite because it is the property
-- that has to hold across the whole schema after every migration has run, and
-- because migration 20 is the first one that ALTERs tables other files create
-- -- so this doubles as proof that the suite applied the files in migration
-- order rather than alphabetically.
DO $$
DECLARE
  rejected boolean := false;
  one_mib  text := repeat('x', 1048576);
BEGIN
  -- Exactly at the ceiling is a legal value, not an off-by-one failure.
  INSERT INTO web_page_cache (url, body) VALUES ('probe-exact', one_mib);
  ASSERT (SELECT octet_length(body) FROM web_page_cache WHERE url = 'probe-exact')
         = 1048576,
    'web_page_cache.body rejected a value of exactly 1 MiB, which the wire carries';
  DELETE FROM web_page_cache WHERE url = 'probe-exact';

  -- One byte over is refused.
  BEGIN
    INSERT INTO web_page_cache (url, body) VALUES ('probe-over', one_mib || 'x');
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected,
    'web_page_cache.body accepted a value over 1 MiB -- it would be stored and never readable';
  DELETE FROM web_page_cache WHERE url = 'probe-over';

  -- octet_length, not length: a multi-byte string that passes a character
  -- count can still be far over the byte ceiling the wire enforces. Two-byte
  -- characters, so 600k of them is 1.2 MiB and must be refused even though it
  -- is well under 1,048,576 CHARACTERS.
  rejected := false;
  BEGIN
    INSERT INTO web_page_cache (url, body) VALUES ('probe-multibyte', repeat('é', 600000));
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected,
    'a 1.2 MiB multi-byte value passed -- the check is counting characters, not bytes';
  DELETE FROM web_page_cache WHERE url = 'probe-multibyte';
END $$;

-- --- length limits count bytes, because every consumer counts bytes -----------
--
-- server_mgmt_token.h holds these fields in fixed byte buffers sized to the
-- same numbers the database checks -- char subject[577] against a 576 limit.
-- While the check counted CHARACTERS, a multi-byte subject could be stored at
-- up to four times the size its own reader can hold. Migration 21 aligns them.
DO $$
DECLARE
  rejected boolean := false;
  -- 300 two-byte characters: 300 characters, 600 bytes. Under a 576-CHARACTER
  -- limit, over a 576-BYTE one. This value is the whole point.
  multibyte text := repeat('é', 300);
BEGIN
  BEGIN
    INSERT INTO server_identity_jti (jti, issuer, kid, audience, subject,
                                     team_id, tier, issued_at, expires_at, consumed_at)
    VALUES ('probe-multibyte', 'iss', 'kid', 'aud', multibyte, 1, 'off', 1, 2, 1);
  EXCEPTION WHEN check_violation THEN rejected := true;
  END;
  ASSERT rejected,
    'server_identity_jti.subject accepted 600 bytes under a 576-byte limit -- still counting characters';
  DELETE FROM server_identity_jti WHERE jti = 'probe-multibyte';

  -- And the limit is still usable at its stated size in bytes: 576 ASCII
  -- characters is 576 bytes and must be accepted, or the fix has quietly
  -- narrowed the column.
  INSERT INTO server_identity_jti (jti, issuer, kid, audience, subject,
                                   team_id, tier, issued_at, expires_at, consumed_at)
  VALUES ('probe-ascii', 'iss', 'kid', 'aud', repeat('a', 576), 1, 'off', 1, 2, 1);
  ASSERT (SELECT octet_length(subject) FROM server_identity_jti WHERE jti = 'probe-ascii') = 576,
    'server_identity_jti.subject rejected 576 ASCII bytes, which is its stated limit';
  DELETE FROM server_identity_jti WHERE jti = 'probe-ascii';
END $$;

-- The columns migration 21 deliberately left alone: an ASCII-only regex makes
-- length() and octet_length() the same function, so changing them would imply
-- a defect that is not there. This asserts that reasoning rather than trusting
-- it -- if someone widens one of these regexes to permit multi-byte, the
-- character-counted limit becomes wrong and this fails.
DO $$
DECLARE bad text;
BEGIN
  SELECT string_agg(format('%s.%s', c.relname, a.attname), ', ')
    INTO bad
    FROM pg_constraint k
    JOIN pg_class c ON c.oid = k.conrelid
    JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY (k.conkey)
   WHERE c.relname IN ('server_identity_jti', 'server_management_jti', 'pki_certs')
     AND pg_get_constraintdef(k.oid) LIKE '%length(%'
     AND pg_get_constraintdef(k.oid) NOT LIKE '%octet_length(%'
     AND pg_get_constraintdef(k.oid) NOT LIKE '%~ ''^[A-Za-z0-9._-]*$''%'
     AND pg_get_constraintdef(k.oid) NOT LIKE '%~ ''^[0-9a-f]*$''%';
  ASSERT bad IS NULL,
    'character-counted limit on a column that permits multi-byte: ' || coalesce(bad, '');
END $$;

-- --- nothing is declared twice -------------------------------------------------
--
-- A table with two CREATE statements is a conflict waiting to happen even when
-- the two agree today. There is no catalog record of "declared twice", so this
-- cannot be asked of the database -- it is checked against the FILES by
-- scripts/check-schema-duplicates.py, which runs in lint. This block is the
-- reminder that the two checks are a pair.

\echo 'WHOLE SCHEMA SUITE PASSED'
