\set ON_ERROR_STOP on
CREATE OR REPLACE FUNCTION pg_temp.p5_assert(v BOOLEAN,msg TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN IF NOT COALESCE(v,false) THEN RAISE EXCEPTION 'assertion failed: %',msg; END IF; END $$;
INSERT INTO kb_team(id,name) VALUES (9501,'p5-a'),(9502,'p5-b');
INSERT INTO kb_team_membership(identity_key,team) VALUES ('oidc:test:lead',9501),('oidc:test:other',9502);
INSERT INTO kb_team_lead(identity_key,team,granted_by) VALUES ('oidc:test:lead',9501,'owner');

BEGIN;
SET LOCAL ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:lead',9501);
SELECT * FROM kb_server_registry_pending('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','srv-a',9501,
  'https://192.168.0.41:7443','server:srv-a:client','server:srv-a:management',
  repeat('1',64),repeat('2',64),600);
DO $$ BEGIN
  BEGIN
    PERFORM * FROM kb_server_registry_pending('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','srv-a',9501,
      'https://192.168.0.99:7443','server:srv-a:client','server:srv-a:management',
      repeat('1',64),repeat('2',64),600);
    RAISE EXCEPTION 'conflicting retry accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;
SELECT * FROM kb_server_registry_pending('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','srv-a',9501,
  'https://192.168.0.41:7443','server:srv-a:client','server:srv-a:management',
  repeat('1',64),repeat('2',64),600);
SELECT * FROM kb_server_registry_finalize('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',repeat('1',64),
  repeat('2',64),'CN=p5-ca','01',repeat('3',64),'CN=p5-ca','02',repeat('4',64));
SELECT * FROM kb_server_registry_finalize('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',repeat('1',64),
  repeat('2',64),'CN=p5-ca','01',repeat('3',64),'CN=p5-ca','02',repeat('4',64));
SELECT * FROM kb_server_registry_pending('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','srv-b',9501,
  'https://192.168.0.42:7443','server:srv-b:client','server:srv-b:management',
  repeat('5',64),repeat('6',64),600);
DO $$ BEGIN
  BEGIN
    PERFORM * FROM kb_server_registry_finalize('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',repeat('5',64),
      repeat('6',64),'CN=p5-ca','03',repeat('3',64),'CN=p5-ca','04',repeat('7',64));
    RAISE EXCEPTION 'duplicate role identity accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END $$;
SELECT pg_temp.p5_assert(status='pending','failed finalize stayed pending')
  FROM kb_server_registry_list(9501) WHERE server_id='srv-b';
SELECT pg_temp.p5_assert(kb_server_registry_heartbeat('srv-a','CN=p5-ca','01',repeat('3',64),'ok','1.0'),'valid heartbeat');
SELECT pg_temp.p5_assert(NOT kb_server_registry_heartbeat('srv-a','CN=p5-ca','ff',repeat('3',64),'bad','1.0'),'wrong serial denied');
SELECT pg_temp.p5_assert(count(*)=2,'team list') FROM kb_server_registry_list(9501);
SELECT pg_temp.p5_assert(count(*)=1,'active snapshot') FROM kb_server_registry_snapshot(9501,'srv-a')
 WHERE enrollment_state='active' AND revoked_at='';
DO $$ BEGIN
  BEGIN PERFORM * FROM kb_server_registry; RAISE EXCEPTION 'direct registry read allowed';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
DO $$ BEGIN
  BEGIN PERFORM * FROM kb_server_registry_list(9502); RAISE EXCEPTION 'cross-team list allowed';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN UPDATE kb_server_registry SET endpoint='https://127.0.0.1' WHERE server_id='srv-a';
    RAISE EXCEPTION 'direct registry update allowed';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET ROLE;
UPDATE kb_enrollments SET revoked_at=now()::TEXT,state='revoked' WHERE fingerprint=repeat('3',64);
SET LOCAL ROLE aimee_kb_runtime;
SELECT pg_temp.p5_assert(NOT kb_server_registry_heartbeat('srv-a','CN=p5-ca','01',repeat('3',64),'bad','1.0'),'revoked heartbeat denied');
ROLLBACK;

SELECT 'p5a server registry pg tests passed' AS result;
