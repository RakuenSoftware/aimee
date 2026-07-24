\set ON_ERROR_STOP on
BEGIN;

INSERT INTO kb_team(name) VALUES('p5b-team-a'),('p5b-team-b');
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,expires_at,legacy,cert_issuer,
                           cert_serial_norm,authority_id)
VALUES
 ('p5-kb-management',repeat('a',64),'01','active',(now()+interval '1 day')::text,0,
  '/CN=p5b-ca','01',repeat('a',32)),
 ('p5-server-management',repeat('b',64),'02','active',(now()+interval '1 day')::text,0,
  '/CN=p5b-ca','02',repeat('b',32)),
 ('p5-server-management',repeat('c',64),'03','active',(now()+interval '1 day')::text,0,
  '/CN=p5b-ca','03',repeat('c',32));

INSERT INTO kb_team_membership(identity_key,team,is_default)
SELECT 'cert:/CN=p5b-ca:01',id,1 FROM kb_team WHERE name='p5b-team-a';

INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
  mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
SELECT 'p5b-server-a','p5b-client-a','p5b-mgmt-a',id,'https://p5b-a.example','active',
  '/CN=p5b-ca','02',repeat('b',64) FROM kb_team WHERE name='p5b-team-a';
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
  mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
SELECT 'p5b-server-b','p5b-client-b','p5b-mgmt-b',id,'https://p5b-b.example','active',
  '/CN=p5b-ca','03',repeat('c',64) FROM kb_team WHERE name='p5b-team-b';

DO $$
DECLARE g1 BIGINT; g2 BIGINT; fp TEXT;
BEGIN
  SELECT generation INTO g1 FROM kb_cert_revocation_generation WHERE singleton=1;
  SELECT revocation_generation,target_mgmt_fingerprint INTO g2,fp
    FROM kb_management_status_lookup('/CN=p5b-ca','01',repeat('a',64),
                                     'p5b-server-a','management.health.v1');
  IF g2<>g1 OR fp<>repeat('b',64) THEN RAISE EXCEPTION 'status lookup mismatch'; END IF;

  BEGIN
    PERFORM * FROM kb_management_status_lookup('/CN=p5b-ca','01',repeat('a',64),
                                               'p5b-server-b','management.health.v1');
    RAISE EXCEPTION 'cross-team lookup allowed';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL;
  END;

  UPDATE kb_enrollments SET state='revoked',revoked_at=pg_now_text()
    WHERE cert_issuer='/CN=p5b-ca' AND cert_serial_norm='01';
  SELECT generation INTO g2 FROM kb_cert_revocation_generation WHERE singleton=1;
  IF g2<>g1+1 THEN RAISE EXCEPTION 'generation did not increment once'; END IF;
  UPDATE kb_enrollments SET state='revoked',revoked_at=revoked_at
    WHERE cert_issuer='/CN=p5b-ca' AND cert_serial_norm='01';
  IF (SELECT generation FROM kb_cert_revocation_generation WHERE singleton=1)<>g2 THEN
    RAISE EXCEPTION 'idempotent revoke incremented generation';
  END IF;
  BEGIN
    UPDATE kb_enrollments SET state='active',revoked_at=''
      WHERE cert_issuer='/CN=p5b-ca' AND cert_serial_norm='01';
    RAISE EXCEPTION 'revoked enrollment resurrected';
  EXCEPTION WHEN object_not_in_prerequisite_state THEN NULL;
  END;
  BEGIN
    PERFORM * FROM kb_management_status_lookup('/CN=p5b-ca','01',repeat('a',64),
                                               'p5b-server-a','management.health.v1');
    RAISE EXCEPTION 'revoked caller allowed';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL;
  END;
END $$;

SET ROLE aimee_kb_status;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM kb_cert_revocation_generation;
    RAISE EXCEPTION 'status role read generation table';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
  BEGIN
    PERFORM * FROM kb_server_registry;
    RAISE EXCEPTION 'status role read registry table';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;
RESET ROLE;

SELECT 'p5b_status_pg17: ok';
ROLLBACK;
