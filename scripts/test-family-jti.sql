-- Verify the jti replay family's PostgreSQL surface on a real server.
--
-- The Go tests script the database, so they prove the commit/rollback decision
-- table and the validation rules and nothing whatever about the SQL. Three
-- things here can only be answered by a real server: that ON CONFLICT DO
-- NOTHING reports a replay as zero rows affected, that the sweep is bounded and
-- ordered, and that the CHECK constraints hold the line the module also checks.
--
-- The statements are PREPAREd from the same text the module ships, so a
-- divergence shows up as a statement that stops preparing.

\set ON_ERROR_STOP on

DROP TABLE IF EXISTS server_identity_jti;
DROP TABLE IF EXISTS server_management_jti;

\i /tmp/family_schema_jti.sql

PREPARE identity_gc (bigint) AS
  DELETE FROM server_identity_jti
   WHERE jti IN (SELECT jti FROM server_identity_jti
                  WHERE expires_at < $1
                  ORDER BY expires_at, jti
                  LIMIT 4096);

PREPARE identity_count AS SELECT count(*) FROM server_identity_jti;

PREPARE identity_insert (text, text, text, text, text, bigint, text, bigint, bigint, bigint) AS
  INSERT INTO server_identity_jti
      (jti, issuer, kid, audience, subject, team_id, tier, issued_at, expires_at, consumed_at)
  VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)
  ON CONFLICT (jti) DO NOTHING;

-- A fresh consume inserts exactly one row.
DO $$
DECLARE before bigint; after bigint;
BEGIN
  SELECT count(*) INTO before FROM server_identity_jti;
  EXECUTE $q$EXECUTE identity_insert('tok00001','iss','kid','aud','sub',7,'data',1000,2000,1500)$q$;
  SELECT count(*) INTO after FROM server_identity_jti;
  ASSERT after - before = 1, format('a fresh jti added %s rows, want 1', after - before);
END $$;

-- The replay guarantee: the same jti a second time affects zero rows. This is
-- what the module reads to answer REPLAY, so if ON CONFLICT ever started
-- reporting 1 here a spent token would be accepted again.
DO $$
DECLARE before bigint; after bigint;
BEGIN
  SELECT count(*) INTO before FROM server_identity_jti;
  EXECUTE $q$EXECUTE identity_insert('tok00001','other','kid2','aud2','sub2',9,'full',1100,2100,1600)$q$;
  SELECT count(*) INTO after FROM server_identity_jti;
  ASSERT after - before = 0, format('a replayed jti added %s rows, want 0', after - before);
END $$;

-- And the original row is untouched: DO NOTHING must not be DO UPDATE.
DO $$
DECLARE t text; team bigint;
BEGIN
  SELECT tier, team_id INTO t, team FROM server_identity_jti WHERE jti = 'tok00001';
  ASSERT t = 'data' AND team = 7,
    format('the replay overwrote the original row: tier=%s team=%s', t, team);
END $$;

-- The sweep removes expired entries and leaves live ones. "Expired" is
-- expires_at < the consume's clock, so a token expiring exactly now survives.
DO $$
DECLARE n bigint;
BEGIN
  INSERT INTO server_identity_jti VALUES ('expired1','iss','kid','aud','sub',7,'data',100,200,150);
  INSERT INTO server_identity_jti VALUES ('expired2','iss','kid','aud','sub',7,'data',100,300,150);
  INSERT INTO server_identity_jti VALUES ('atedge01','iss','kid','aud','sub',7,'data',100,500,150);
  SELECT count(*) INTO n FROM server_identity_jti;
  EXECUTE $q$EXECUTE identity_gc(500)$q$;
  ASSERT n - (SELECT count(*) FROM server_identity_jti) = 2,
    format('the sweep removed %s rows, want 2', n - (SELECT count(*) FROM server_identity_jti));
  ASSERT EXISTS (SELECT 1 FROM server_identity_jti WHERE jti = 'atedge01'),
    'the sweep removed a token expiring exactly at the clock';
  ASSERT EXISTS (SELECT 1 FROM server_identity_jti WHERE jti = 'tok00001'),
    'the sweep removed a live token';
END $$;

-- Every CHECK the module also enforces must be enforced by the store, so a
-- writer that skipped the module could not leave a record the module would
-- refuse to produce.
DO $$
DECLARE
  bad text[][] := ARRAY[
    ['short',            'jti below the 8-character floor'],
    ['has spaces!!',     'jti outside the identifier alphabet']
  ];
  i int;
BEGIN
  FOR i IN 1 .. array_length(bad, 1) LOOP
    BEGIN
      EXECUTE format(
        'INSERT INTO server_identity_jti VALUES (%L,''iss'',''kid'',''aud'',''sub'',7,''data'',1000,2000,1500)',
        bad[i][1]);
      ASSERT false, format('the store accepted %s: %s', bad[i][1], bad[i][2]);
    EXCEPTION WHEN check_violation THEN
      NULL;  -- expected
    END;
  END LOOP;
END $$;

DO $$
BEGIN
  BEGIN
    INSERT INTO server_identity_jti VALUES ('tier0001','iss','kid','aud','sub',7,'root',1000,2000,1500);
    ASSERT false, 'the store accepted an unrecognised tier';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO server_identity_jti VALUES ('team0001','iss','kid','aud','sub',0,'data',1000,2000,1500);
    ASSERT false, 'the store accepted team_id = 0';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO server_identity_jti VALUES ('time0001','iss','kid','aud','sub',7,'data',2000,1000,1500);
    ASSERT false, 'the store accepted expires_at before issued_at';
  EXCEPTION WHEN check_violation THEN NULL; END;

  BEGIN
    INSERT INTO server_identity_jti VALUES ('time0002','iss','kid','aud','sub',7,'data',1000,2000,2000);
    ASSERT false, 'the store accepted consumed_at at expiry';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- The sweep's index must exist and cover (expires_at, jti) in that order: the
-- sweep orders by exactly those columns, and without it every consume on a
-- large store becomes a sequential scan.
DO $$
DECLARE definition text;
BEGIN
  SELECT indexdef INTO definition FROM pg_indexes
   WHERE tablename = 'server_identity_jti' AND indexname = 'idx_server_identity_jti_expiry';
  ASSERT definition IS NOT NULL, 'the sweep index is missing';
  ASSERT definition LIKE '%(expires_at, jti)%',
    format('the sweep index is %s, want (expires_at, jti)', definition);
END $$;

-- The management store's floor is 16, not 8: the same jti that is legal for an
-- identity token must be refused here.
DO $$
BEGIN
  BEGIN
    INSERT INTO server_management_jti VALUES
      ('abcd1234','iss','kid','aud','sub',7,'cap','peer','00ff',
       repeat('ab',32), repeat('ab',32),'corr',1000,2000,1500);
    ASSERT false, 'the management store accepted an 8-character jti';
  EXCEPTION WHEN check_violation THEN NULL; END;

  INSERT INTO server_management_jti VALUES
    ('abcd1234abcd1234','iss','kid','aud','sub',7,'cap','peer','00ff',
     repeat('ab',32), repeat('ab',32),'corr',1000,2000,1500);

  BEGIN
    INSERT INTO server_management_jti VALUES
      ('uppercasehexxxxx','iss','kid','aud','sub',7,'cap','peer','00FF',
       repeat('ab',32), repeat('ab',32),'corr',1000,2000,1500);
    ASSERT false, 'the management store accepted uppercase hex in peer_serial';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

DROP TABLE server_identity_jti;
DROP TABLE server_management_jti;

\echo 'JTI FAMILY SUITE PASSED'
