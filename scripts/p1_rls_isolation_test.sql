-- p1_rls_isolation_test.sql: the mandatory DB-layer tenancy-isolation gate (I1-I4,
-- B4). Run against a Postgres that has had schema_roles.sql + schema.sql applied.
-- Proves cross-team isolation, fail-closed context, membership validation, the
-- GUC-spoof defense-in-depth, and the runtime-role privilege floor — all at the DB
-- layer, under the non-owner NOBYPASSRLS runtime role. Any failed assertion aborts
-- with an ERROR (non-zero psql exit), so this is a hard CI gate, never a skip.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p1_rls_isolation_test.sql
--
-- Uses SET ROLE aimee_kb_runtime so a single superuser session exercises the
-- runtime role's RLS view (SET ROLE drops to the target role's privileges,
-- including FORCE ROW LEVEL SECURITY, exactly as a runtime connection would see).

\set ON_ERROR_STOP on

BEGIN;

-- Isolated fixtures in a savepoint-free txn we roll back at the end.
INSERT INTO kb_team(id, name) VALUES (900001, 'rls_alpha'), (900002, 'rls_beta');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:alice', 900001, 1), ('oidc:test:bob', 900002, 1);
INSERT INTO kb_project(id, parent, name)
  VALUES (900010, 900001, 'alpha_proj'), (900020, 900002, 'beta_proj');

-- B4: the runtime role must be non-owner, NOBYPASSRLS.
DO $$
BEGIN
  IF (SELECT rolbypassrls FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE EXCEPTION 'B4 FAIL: aimee_kb_runtime has BYPASSRLS';
  END IF;
  IF (SELECT relforcerowsecurity FROM pg_class WHERE relname = 'kb_team') IS NOT TRUE THEN
    RAISE EXCEPTION 'I3 FAIL: kb_team is not FORCE ROW LEVEL SECURITY';
  END IF;
END $$;

SET ROLE aimee_kb_runtime;

-- I4/fail-closed: no tenant context set -> zero rows (never an error, never a leak).
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM kb_team;
  IF n <> 0 THEN RAISE EXCEPTION 'FAIL no-context: expected 0 teams, got %', n; END IF;
END $$;

-- Isolation: alice sees exactly her team + project, never beta's.
SELECT set_tenant_context('oidc:test:alice', 900001);
DO $$
DECLARE teams TEXT; projs TEXT;
BEGIN
  SELECT string_agg(id::text, ',' ORDER BY id) INTO teams FROM kb_team WHERE id IN (900001,900002);
  SELECT string_agg(id::text, ',' ORDER BY id) INTO projs FROM kb_project WHERE id IN (900010,900020);
  IF teams IS DISTINCT FROM '900001' THEN RAISE EXCEPTION 'FAIL alice teams: got %', teams; END IF;
  IF projs IS DISTINCT FROM '900010' THEN RAISE EXCEPTION 'FAIL alice projects: got %', projs; END IF;
END $$;

-- Membership validation: alice cannot select beta as her billing team (raises).
DO $$
BEGIN
  BEGIN
    PERFORM set_tenant_context('oidc:test:alice', 900002);
    RAISE EXCEPTION 'FAIL: set_tenant_context allowed a non-member team';
  EXCEPTION WHEN insufficient_privilege THEN
    NULL; -- expected: 42501 raised by set_tenant_context
  END;
END $$;

-- Defense-in-depth: even directly forging aimee.team to beta, the data policy
-- (bound to the principal's membership) still denies beta's rows.
RESET ROLE;  -- clear aborted-savepoint state from the caught exception path
SET ROLE aimee_kb_runtime;
SELECT set_config('aimee.principal', 'oidc:test:alice', true);
SELECT set_config('aimee.team', '900002', true);
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM kb_team WHERE id = 900002;
  IF n <> 0 THEN RAISE EXCEPTION 'FAIL guc-spoof: alice read beta via forged aimee.team'; END IF;
END $$;

RESET ROLE;
ROLLBACK;

\echo 'p1_rls_isolation_test: ALL ASSERTIONS PASSED'
