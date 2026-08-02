\set ON_ERROR_STOP on
BEGIN;

INSERT INTO kb_team(name) VALUES('p5c3-team');
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,expires_at,legacy,cert_issuer,
                           cert_serial_norm,authority_id)
VALUES
 ('service:aimee-server',repeat('e',64),'02','active',(now()+interval '1 day')::text,0,
  '/CN=p5c3-server-ca','02',repeat('e',32)),
 ('p5-kb-management',repeat('a',64),'01','active',(now()+interval '1 day')::text,0,
  '/CN=p5c3-kb-ca','01',repeat('a',32));
INSERT INTO kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
  client_issuer,client_serial_norm,client_fingerprint)
SELECT 'p5c3-server','p5c3-client','p5c3-mgmt',id,'https://p5c3.example','active',
  '/CN=p5c3-server-ca','02',repeat('e',64) FROM kb_team WHERE name='p5c3-team';

DO $$
DECLARE r BOOLEAN; g BIGINT;
BEGIN
  SELECT revoked,generation INTO r,g FROM kb_management_action_checkpoint(
    '/CN=p5c3-server-ca','02',repeat('e',64),'p5c3-server',
    '/CN=p5c3-kb-ca','01',repeat('a',64),1);
  IF r OR g<1 THEN RAISE EXCEPTION 'active checkpoint mismatch'; END IF;
  BEGIN
    PERFORM * FROM kb_management_action_checkpoint(
      '/CN=p5c3-server-ca','02',repeat('f',64),'p5c3-server',
      '/CN=p5c3-kb-ca','01',repeat('a',64),1);
    RAISE EXCEPTION 'wrong server peer admitted';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL;
  END;
  BEGIN
    PERFORM * FROM kb_management_action_checkpoint(
      '/CN=p5c3-server-ca','02',repeat('e',64),'p5c3-server',
      '/CN=p5c3-kb-ca','01',repeat('a',64),g+1);
    RAISE EXCEPTION 'future generation admitted';
  EXCEPTION WHEN unique_violation THEN NULL;
  END;
  UPDATE kb_enrollments SET state='revoked',revoked_at=pg_now_text()
   WHERE scope='p5-kb-management' AND cert_issuer='/CN=p5c3-kb-ca' AND cert_serial_norm='01';
  SELECT revoked,generation INTO r,g FROM kb_management_action_checkpoint(
    '/CN=p5c3-server-ca','02',repeat('e',64),'p5c3-server',
    '/CN=p5c3-kb-ca','01',repeat('a',64),g);
  IF NOT r THEN RAISE EXCEPTION 'revocation not conveyed'; END IF;
END $$;

SET ROLE aimee_kb_status;
SELECT revoked,generation FROM kb_management_action_checkpoint(
  '/CN=p5c3-server-ca','02',repeat('e',64),'p5c3-server',
  '/CN=p5c3-kb-ca','01',repeat('a',64),1);
DO $$ BEGIN
  BEGIN
    PERFORM * FROM kb_server_registry;
    RAISE EXCEPTION 'status role read registry directly';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
END $$;
RESET ROLE;

SELECT 'p5c3_action_checkpoint_pg17: ok';
ROLLBACK;
