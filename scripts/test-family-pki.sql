-- Verify the PKI family on a real server.
--
-- The Go tests script the database, so what only a real server settles is here:
-- that the ramp is genuinely a single row, that the conditional advance really
-- matches nothing when the roster hash has moved, and that greatest() keeps a
-- presentation timestamp from going backwards.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS pki_certs;
DROP TABLE IF EXISTS pki_mtls_ramp;

\i /tmp/family_schema_pki.sql

-- --- the ramp is one row, in one of two states --------------------------------

DO $$
BEGIN
  INSERT INTO pki_mtls_ramp (singleton, ramp_state, roster_hash, last_advance_ts)
       VALUES (true, 1, repeat('a', 64), 0);

  BEGIN
    INSERT INTO pki_mtls_ramp (singleton, ramp_state, roster_hash, last_advance_ts)
         VALUES (true, 2, repeat('b', 64), 100);
    ASSERT false, 'a second ramp row was accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;

  -- there is no state 0 and no state 3
  BEGIN
    UPDATE pki_mtls_ramp SET ramp_state = 0;
    ASSERT false, 'ramp_state 0 was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    UPDATE pki_mtls_ramp SET ramp_state = 3;
    ASSERT false, 'ramp_state 3 was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  -- the hash column holds a sha256, not any string
  BEGIN
    UPDATE pki_mtls_ramp SET roster_hash = 'short';
    ASSERT false, 'a 5-character roster hash was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    UPDATE pki_mtls_ramp SET roster_hash = upper(repeat('a', 64));
    ASSERT false, 'an uppercase roster hash was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- --- the conditional advance ---------------------------------------------------

-- The advance carries the hash in its WHERE clause. That is what stops a
-- certificate added between the readiness check and the write from being
-- enforced against: if the roster moved, the statement matches nothing.
DO $$
DECLARE changed bigint; state smallint;
BEGIN
  UPDATE pki_mtls_ramp SET ramp_state = 1, roster_hash = repeat('a', 64);

  -- the hash the caller judged ready no longer matches: no advance
  UPDATE pki_mtls_ramp
     SET ramp_state = 2, last_advance_ts = 1000
   WHERE singleton AND ramp_state = 1 AND roster_hash = repeat('c', 64);
  GET DIAGNOSTICS changed = ROW_COUNT;
  ASSERT changed = 0, format('a stale hash advanced the ramp (%s rows)', changed);
  SELECT ramp_state INTO state FROM pki_mtls_ramp;
  ASSERT state = 1, format('ramp_state = %s, want it still observing', state);

  -- the matching hash advances exactly one row
  UPDATE pki_mtls_ramp
     SET ramp_state = 2, last_advance_ts = 1000
   WHERE singleton AND ramp_state = 1 AND roster_hash = repeat('a', 64);
  GET DIAGNOSTICS changed = ROW_COUNT;
  ASSERT changed = 1, format('the advance changed %s rows, want 1', changed);

  -- and advancing again is a no-op: ramp_state = 1 no longer matches
  UPDATE pki_mtls_ramp
     SET ramp_state = 2, last_advance_ts = 2000
   WHERE singleton AND ramp_state = 1 AND roster_hash = repeat('a', 64);
  GET DIAGNOSTICS changed = ROW_COUNT;
  ASSERT changed = 0, 'an already-enforcing ramp advanced again';
  SELECT last_advance_ts INTO state FROM pki_mtls_ramp;
  ASSERT state = 1000, 'the second advance overwrote the first advance time';
END $$;

-- --- the roster ----------------------------------------------------------------

-- A serial identifies one certificate. A second certificate claiming the same
-- serial is a different key claiming an identity, not a refresh.
DO $$
BEGIN
  INSERT INTO pki_certs (serial, cn, issued_at, expires_at) VALUES ('AA', 'node-a', 10, 5000);
  BEGIN
    INSERT INTO pki_certs (serial, cn, issued_at, expires_at) VALUES ('AA', 'node-b', 20, 6000);
    ASSERT false, 'two certificates shared one serial';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;

-- The live roster is unrevoked and unexpired, ordered by (serial, cn). The hash
-- is taken over exactly this set, so what it excludes is load-bearing.
DO $$
DECLARE live text[];
BEGIN
  DELETE FROM pki_certs;
  INSERT INTO pki_certs (serial, cn, issued_at, expires_at, revoked) VALUES
    ('BB', 'node-b', 10, 5000, false),
    ('AA', 'node-a', 10, 0,    false),   -- 0 means never expires
    ('CC', 'node-c', 10, 100,  false),   -- expired at now=1000
    ('DD', 'node-d', 10, 5000, true);    -- revoked

  SELECT array_agg(serial ORDER BY serial, cn) INTO live
    FROM pki_certs
   WHERE NOT revoked AND (expires_at = 0 OR expires_at > 1000);
  ASSERT live = ARRAY['AA','BB'],
    format('live roster = %s, want AA and BB only', live);
END $$;

-- --- presentation never goes backwards -----------------------------------------

DO $$
DECLARE seen bigint; changed bigint;
BEGIN
  DELETE FROM pki_certs;
  INSERT INTO pki_certs (serial, cn, issued_at, expires_at, last_presented_at)
       VALUES ('AA', 'node-a', 10, 5000, 900);

  -- a later presentation moves it forward
  UPDATE pki_certs SET last_presented_at = greatest(last_presented_at, 1000)
   WHERE serial = 'AA' AND NOT revoked AND (expires_at = 0 OR expires_at > 1000);
  SELECT last_presented_at INTO seen FROM pki_certs WHERE serial = 'AA';
  ASSERT seen = 1000, format('last_presented_at = %s, want 1000', seen);

  -- an out-of-order one does not move it back
  UPDATE pki_certs SET last_presented_at = greatest(last_presented_at, 500)
   WHERE serial = 'AA' AND NOT revoked AND (expires_at = 0 OR expires_at > 500);
  SELECT last_presented_at INTO seen FROM pki_certs WHERE serial = 'AA';
  ASSERT seen = 1000, format('an older presentation moved the mark to %s', seen);

  -- a revoked certificate matches nothing, so the module reports failure
  UPDATE pki_certs SET revoked = true WHERE serial = 'AA';
  UPDATE pki_certs SET last_presented_at = greatest(last_presented_at, 2000)
   WHERE serial = 'AA' AND NOT revoked AND (expires_at = 0 OR expires_at > 2000);
  GET DIAGNOSTICS changed = ROW_COUNT;
  ASSERT changed = 0, 'a revoked certificate was refreshed by a presentation';

  -- and so does an expired one
  UPDATE pki_certs SET revoked = false, expires_at = 100 WHERE serial = 'AA';
  UPDATE pki_certs SET last_presented_at = greatest(last_presented_at, 3000)
   WHERE serial = 'AA' AND NOT revoked AND (expires_at = 0 OR expires_at > 3000);
  GET DIAGNOSTICS changed = ROW_COUNT;
  ASSERT changed = 0, 'an expired certificate was refreshed by a presentation';
END $$;

-- The list index must exist, and it must be total: issued_at alone is not
-- unique, so a LIMIT over it would not be a stable page.
DO $$
DECLARE definition text;
BEGIN
  SELECT indexdef INTO definition FROM pg_indexes
   WHERE tablename = 'pki_certs' AND indexname = 'idx_pki_certs_issued';
  ASSERT definition IS NOT NULL, 'the list index is missing';
  ASSERT definition LIKE '%serial%',
    format('the list index is %s, want serial as a tiebreaker', definition);
END $$;

DROP TABLE pki_certs;
DROP TABLE pki_mtls_ramp;

\echo 'PKI FAMILY SUITE PASSED'
