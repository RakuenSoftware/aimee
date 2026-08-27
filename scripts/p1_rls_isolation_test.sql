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
INSERT INTO kb_team(id, name) VALUES (900001, 'rls_alpha'), (900002, 'rls_beta'), (900003, 'rls_gamma');
-- alice is a member of alpha (default) AND gamma; bob is beta only.
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:alice', 900001, 1), ('oidc:test:alice', 900003, 0),
         ('oidc:test:bob', 900002, 1);
INSERT INTO kb_project(id, parent, name, access_mode)
  VALUES (900010, 900001, 'alpha_proj', 'team-open'),
         (900011, 900001, 'alpha_restricted', 'restricted'),  -- alice NOT a member
         (900020, 900002, 'beta_proj', 'team-open'),
         (900030, 900003, 'gamma_proj', 'team-open');

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

-- Project access model under aimee.team=alpha: alice sees the team-open alpha
-- project, but NOT the restricted alpha project (she is not a project member) and
-- NOT her gamma project (a single-team request scoped to alpha must not read her
-- OTHER team's rows).
DO $$
DECLARE projs TEXT;
BEGIN
  SELECT string_agg(id::text, ',' ORDER BY id) INTO projs FROM kb_project
    WHERE id IN (900010, 900011, 900020, 900030);
  IF projs IS DISTINCT FROM '900010' THEN
    RAISE EXCEPTION 'FAIL project access model (aimee.team=alpha): expected 900010, got %', projs;
  END IF;
END $$;

-- Switching the billing team to gamma reveals the gamma project (and only it).
SELECT set_tenant_context('oidc:test:alice', 900003);
DO $$
DECLARE projs TEXT;
BEGIN
  SELECT string_agg(id::text, ',' ORDER BY id) INTO projs FROM kb_project
    WHERE id IN (900010, 900011, 900020, 900030);
  IF projs IS DISTINCT FROM '900030' THEN
    RAISE EXCEPTION 'FAIL aimee.team scoping (=gamma): expected 900030, got %', projs;
  END IF;
END $$;
SELECT set_tenant_context('oidc:test:alice', 900001);  -- restore alpha context

-- A restricted project becomes visible once the caller is an explicit member.
RESET ROLE;
INSERT INTO kb_project_membership(project, identity_key) VALUES (900011, 'oidc:test:alice');
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:alice', 900001);
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM kb_project WHERE id = 900011;
  IF n <> 1 THEN RAISE EXCEPTION 'FAIL restricted project not visible to its member (got %)', n; END IF;
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

-- Self-escalation is denied: read-only (FOR SELECT) policies mean the runtime role
-- cannot INSERT its own membership into a team, nor self-grant admin, even though
-- it holds table-level INSERT. Under FORCE RLS an INSERT with no permissive write
-- policy raises. (Fixtures were seeded as superuser; the runtime role must not be
-- able to add its own rows.)
SET ROLE aimee_kb_runtime;
SELECT set_config('aimee.principal', 'oidc:test:alice', true);
DO $$
BEGIN
  BEGIN
    INSERT INTO kb_team_membership(identity_key, team) VALUES ('oidc:test:alice', 900002);
    RAISE EXCEPTION 'FAIL: runtime self-enrolled into a team (RLS write not denied)';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;  -- expected: RLS denies
  BEGIN
    INSERT INTO kb_admin_grant(identity_key, source) VALUES ('oidc:test:alice', 'oidc');
    RAISE EXCEPTION 'FAIL: runtime self-granted admin (RLS write not denied)';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;  -- expected: RLS denies
END $$;

-- Admin-gated writes (slice 4). The bootstrap OWNER can create a team and mint the
-- first admin grant; a granted admin can then create teams; a non-admin cannot.
SET ROLE aimee_kb_runtime;
-- (a) owner bootstrap: create a team + grant alice admin
SELECT set_tenant_context('owner', 0);
INSERT INTO kb_team(id, name) VALUES (900100, 'owner_made');
INSERT INTO kb_admin_grant(identity_key, source) VALUES ('oidc:test:alice', 'oidc');
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM kb_team WHERE id = 900100;  -- owner sees its own write via... membership? no
  -- owner is not a member, so RLS read hides it; assert the INSERT itself succeeded (no exception).
  NULL;
END $$;
-- (b) alice is now an admin: she can create a team
SELECT set_tenant_context('oidc:test:alice', 900001);
INSERT INTO kb_team(id, name) VALUES (900101, 'alice_admin_made');
-- (c) bob is NOT an admin: his team-create is denied by the WITH CHECK policy
SELECT set_tenant_context('oidc:test:bob', 900002);
DO $$
BEGIN
  BEGIN
    INSERT INTO kb_team(id, name) VALUES (900102, 'bob_denied');
    RAISE EXCEPTION 'FAIL: non-admin bob created a team';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: WITH CHECK denies
  END;
END $$;

-- ---------------------------------------------------------------------------
-- memory_workspace_readable(): the predicate future memory policies will call.
-- No memory table has RLS enabled yet, so these assert the RESOLVER's behaviour
-- directly. Each case is one rule from the ownership model.
INSERT INTO memory_workspace_owner(workspace, owner_kind, owner_id) VALUES
  ('ws_alice_owned', 'principal', 'oidc:test:alice'),
  ('ws_alpha_team',  'team',      '900001'),
  ('ws_beta_team',   'team',      '900002'),
  ('ws_org_owned',   'org',       'org-1'),
  ('ws_typo_kind',   'principle', 'oidc:test:alice');

SELECT set_tenant_context('oidc:test:alice', 900001);
DO $$
BEGIN
  -- Workspace-less is global scope: readable by an authenticated principal.
  IF NOT memory_workspace_readable('') THEN
    RAISE EXCEPTION 'FAIL: empty workspace not readable as global scope';
  END IF;
  IF NOT memory_workspace_readable(NULL) THEN
    RAISE EXCEPTION 'FAIL: NULL workspace not readable as global scope';
  END IF;
  -- Principal-owned: the owning principal reads it.
  IF NOT memory_workspace_readable('ws_alice_owned') THEN
    RAISE EXCEPTION 'FAIL: owner principal cannot read own workspace';
  END IF;
  -- Team-owned: alice is a member of alpha (900001), not beta (900002).
  IF NOT memory_workspace_readable('ws_alpha_team') THEN
    RAISE EXCEPTION 'FAIL: team member cannot read team workspace';
  END IF;
  IF memory_workspace_readable('ws_beta_team') THEN
    RAISE EXCEPTION 'FAIL: non-member read a foreign team workspace';
  END IF;
  -- An unmapped workspace has no owner row: fail closed.
  IF memory_workspace_readable('ws_never_mapped') THEN
    RAISE EXCEPTION 'FAIL: unmapped workspace was readable';
  END IF;
  -- org has no membership source in this schema yet: must fail closed.
  IF memory_workspace_readable('ws_org_owned') THEN
    RAISE EXCEPTION 'FAIL: org-owned workspace readable without a membership source';
  END IF;
  -- An owner_kind the resolver does not know must fail closed, not open.
  IF memory_workspace_readable('ws_typo_kind') THEN
    RAISE EXCEPTION 'FAIL: unknown owner_kind was readable';
  END IF;
END $$;

-- bob is beta-only: the team cases invert for him.
SELECT set_tenant_context('oidc:test:bob', 900002);
DO $$
BEGIN
  IF memory_workspace_readable('ws_alpha_team') THEN
    RAISE EXCEPTION 'FAIL: bob read alpha team workspace';
  END IF;
  IF NOT memory_workspace_readable('ws_beta_team') THEN
    RAISE EXCEPTION 'FAIL: bob cannot read his own team workspace';
  END IF;
  IF memory_workspace_readable('ws_alice_owned') THEN
    RAISE EXCEPTION 'FAIL: bob read alice''s principal-owned workspace';
  END IF;
END $$;

-- Fail-closed with no tenant context: even global scope is unreadable.
RESET ROLE;
SET ROLE aimee_kb_runtime;
SELECT set_config('aimee.principal', '', true);
DO $$
BEGIN
  IF memory_workspace_readable('') THEN
    RAISE EXCEPTION 'FAIL: global scope readable with no principal set';
  END IF;
  IF memory_workspace_readable('ws_alice_owned') THEN
    RAISE EXCEPTION 'FAIL: owned workspace readable with no principal set';
  END IF;
END $$;

RESET ROLE;
ROLLBACK;

\echo 'p1_rls_isolation_test: ALL ASSERTIONS PASSED'
