-- Verify the management families' PostgreSQL surface on a real server.
--
-- The Go tests script the database, so what they cannot answer is whether the
-- schema actually enforces the invariants the module relies on: that each
-- singleton table holds at most one row, that the high-water mark's seed exists,
-- and that the monotonic UPDATE really matches no row when asked to go
-- backwards. Those are the checks here.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS server_management_jwks_cache;
DROP TABLE IF EXISTS server_mgmt_nonce;
DROP TABLE IF EXISTS server_mgmt_status_hwm;

\i /tmp/family_schema_jwks.sql
\i /tmp/family_schema_nonce.sql

-- --- the JWKS cache is a singleton -------------------------------------------

DO $$
BEGIN
  INSERT INTO server_management_jwks_cache
      (singleton, generation, valid_from, valid_until, jwks_bytes, envelope_bytes,
       envelope_sha256, manifest_sha256, trust_bundle_sha256, fetched_at)
  VALUES (true, 1, 100, 200, '\x01020304'::bytea, '{"keys":[]}',
          decode(repeat('aa',32),'hex'), decode(repeat('bb',32),'hex'),
          decode(repeat('cc',32),'hex'), 150);

  BEGIN
    INSERT INTO server_management_jwks_cache
        (singleton, generation, valid_from, valid_until, jwks_bytes, envelope_bytes,
         envelope_sha256, manifest_sha256, trust_bundle_sha256, fetched_at)
    VALUES (true, 1, 300, 400, '\x05'::bytea, 'other',
            decode(repeat('11',32),'hex'), decode(repeat('22',32),'hex'),
            decode(repeat('33',32),'hex'), 350);
    ASSERT false, 'a second JWKS row was accepted -- the cache is not a singleton';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;

-- The digests must be exactly 32 bytes, so a hex string stored by mistake (64
-- bytes of ASCII) is refused rather than silently kept.
DO $$
BEGIN
  BEGIN
    UPDATE server_management_jwks_cache SET envelope_sha256 = repeat('aa',32)::bytea;
    ASSERT false, 'a 64-byte hex string was accepted as a 32-byte digest';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- The module reads with `WHERE singleton`, which must find the row.
DO $$
DECLARE found bigint;
BEGIN
  SELECT generation INTO found FROM server_management_jwks_cache WHERE singleton;
  ASSERT found = 1, format('WHERE singleton read generation %s', found);
END $$;

-- --- the high-water mark is seeded and monotonic ------------------------------

-- The seed is what makes the store usable from cold: hwm_read reports a failure
-- when there is no row, and both hwm_set and the consume path require their
-- UPDATE to affect exactly one row.
DO $$
DECLARE n bigint;
BEGIN
  SELECT count(*) INTO n FROM server_mgmt_status_hwm;
  ASSERT n = 1, format('the high-water mark table holds %s rows, want exactly 1 (seeded)', n);
  SELECT generation INTO n FROM server_mgmt_status_hwm WHERE singleton;
  ASSERT n = 0, format('the seed generation is %s, want 0', n);
END $$;

-- Re-applying the schema must not duplicate or reset the seed.
DO $$
DECLARE n bigint;
BEGIN
  UPDATE server_mgmt_status_hwm SET generation = 7 WHERE singleton;
  INSERT INTO server_mgmt_status_hwm (singleton, generation) VALUES (true, 0)
    ON CONFLICT (singleton) DO NOTHING;
  SELECT count(*) INTO n FROM server_mgmt_status_hwm;
  ASSERT n = 1, format('re-seeding produced %s rows', n);
  SELECT generation INTO n FROM server_mgmt_status_hwm WHERE singleton;
  ASSERT n = 7, format('re-seeding reset the generation to %s, want it left at 7', n);
END $$;

-- hwm_set's monotonicity is in the WHERE clause, not in the module: going
-- backwards must match NO row, which is what the module reads to refuse.
PREPARE hwm_set (bigint) AS
  UPDATE server_mgmt_status_hwm
     SET generation = $1
   WHERE singleton AND generation <= $1;

DO $$
DECLARE n bigint;
BEGIN
  -- forward
  EXECUTE $q$EXECUTE hwm_set(10)$q$;
  SELECT generation INTO n FROM server_mgmt_status_hwm WHERE singleton;
  ASSERT n = 10, format('forward set left generation at %s', n);

  -- backward: matches nothing, leaves the mark alone
  EXECUTE $q$EXECUTE hwm_set(3)$q$;
  SELECT generation INTO n FROM server_mgmt_status_hwm WHERE singleton;
  ASSERT n = 10, format('a backward set moved the mark to %s', n);

  -- equal: still matches, still one row
  EXECUTE $q$EXECUTE hwm_set(10)$q$;
  SELECT generation INTO n FROM server_mgmt_status_hwm WHERE singleton;
  ASSERT n = 10, format('an equal set left generation at %s', n);
END $$;

-- The consume path bumps with greatest(), which must never lower the mark even
-- though the module has already checked.
DO $$
DECLARE n bigint;
BEGIN
  UPDATE server_mgmt_status_hwm SET generation = greatest(generation, 4) WHERE singleton;
  SELECT generation INTO n FROM server_mgmt_status_hwm WHERE singleton;
  ASSERT n = 10, format('greatest() lowered the mark to %s', n);

  UPDATE server_mgmt_status_hwm SET generation = greatest(generation, 20) WHERE singleton;
  SELECT generation INTO n FROM server_mgmt_status_hwm WHERE singleton;
  ASSERT n = 20, format('greatest() did not raise the mark: %s', n);
END $$;

DO $$
BEGIN
  BEGIN
    UPDATE server_mgmt_status_hwm SET generation = -1 WHERE singleton;
    ASSERT false, 'a negative generation was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- --- the challenge store ------------------------------------------------------

-- The nonce is the primary key and is exactly 32 bytes, so a truncated one
-- cannot be stored and later matched against a full one.
DO $$
BEGIN
  INSERT INTO server_mgmt_nonce VALUES
    (decode(repeat('ab',32),'hex'),'iss','00ff','fp','binding','server-1','attest',2000);

  BEGIN
    INSERT INTO server_mgmt_nonce VALUES
      (decode(repeat('ab',32),'hex'),'other','beef','fp2','b2','server-2','attest',3000);
    ASSERT false, 'a duplicate nonce was accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;

  BEGIN
    INSERT INTO server_mgmt_nonce VALUES
      (decode(repeat('ab',16),'hex'),'iss','00ff','fp','binding','server-1','attest',2000);
    ASSERT false, 'a 16-byte nonce was accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- The sweep deletes strictly-expired rows, so a challenge expiring exactly at
-- the caller's clock survives -- issue and consume agree on that boundary.
DO $$
DECLARE n bigint;
BEGIN
  INSERT INTO server_mgmt_nonce VALUES
    (decode(repeat('cd',32),'hex'),'iss','00ff','fp','binding','server-1','attest',1000);
  DELETE FROM server_mgmt_nonce WHERE expires_at < 2000;
  ASSERT NOT EXISTS (SELECT 1 FROM server_mgmt_nonce WHERE nonce = decode(repeat('cd',32),'hex')),
    'the sweep left an expired challenge';
  ASSERT EXISTS (SELECT 1 FROM server_mgmt_nonce WHERE nonce = decode(repeat('ab',32),'hex')),
    'the sweep removed a challenge expiring exactly at the clock';
END $$;

DO $$
DECLARE definition text;
BEGIN
  SELECT indexdef INTO definition FROM pg_indexes
   WHERE tablename = 'server_mgmt_nonce' AND indexname = 'idx_server_mgmt_nonce_expiry';
  ASSERT definition IS NOT NULL, 'the sweep index is missing';
END $$;

DROP TABLE server_management_jwks_cache;
DROP TABLE server_mgmt_nonce;
DROP TABLE server_mgmt_status_hwm;

\echo 'MANAGEMENT FAMILY SUITE PASSED'
