-- p10_vault_rls_test.sql: the mandatory DB-layer credential-vault isolation gate
-- (P10 slice 2). Run against a Postgres that has had schema_roles.sql + schema.sql +
-- schema_grants.sql applied. Proves, at the DB layer under the non-owner NOBYPASSRLS
-- runtime role:
--   * team-scoped read isolation (a member reads ONLY its own team's vault rows),
--   * the CRITICAL v2 invariant: a platform-scoped row (team_id IS NULL, e.g. the kb
--     CA key) is NEVER a tenant read — only an org-admin can see it,
--   * cross-team isolation is symmetric,
--   * the runtime role cannot forge a vault row out of band (writes go only through the
--     SECURITY DEFINER vault functions),
--   * the version pointer is monotonic (org_vault_put advances 1 -> 2).
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p10_vault_rls_test.sql
--
-- Uses SET ROLE aimee_kb_runtime so a single superuser session exercises the runtime
-- role's RLS view. The whole test is one transaction (so set_tenant_context's
-- transaction-local principal GUC persists across statements) and is rolled back.

\set ON_ERROR_STOP on

BEGIN;

-- Seed as the owner principal. The vault definer functions run as the (superuser) owner
-- and bypass RLS for their internal writes, exactly like P3a's metering functions.
SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (940001, 'p10_alpha'), (940002, 'p10_beta');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:member_a', 940001, 1), ('oidc:test:member_b', 940002, 1);

-- Two team-scoped secrets (one per team) + one platform-scoped CA-key-style row
-- (team_id NULL). The bytea envelope fields are placeholders — this gate tests row
-- VISIBILITY, not decryption (the C test proves the envelope round-trip).
SELECT org_vault_put('team:940001:provider:anthropic', 940001, 'claude', 'api_key', 1,
  '\xDEADBEEF'::bytea, '\x0102030405060708090A0B0C'::bytea, '\xAABBCC'::bytea, '\x1122334455667788'::bytea);
SELECT org_vault_put('team:940002:provider:anthropic', 940002, 'claude', 'api_key', 1,
  '\xDEADBEEF'::bytea, '\x0102030405060708090A0B0C'::bytea, '\xAABBCC'::bytea, '\x1122334455667788'::bytea);
SELECT org_vault_put('org:pki:ca-key', NULL, 'kb', 'ca_key', 1,
  '\xDEADBEEF'::bytea, '\x0102030405060708090A0B0C'::bytea, '\xAABBCC'::bytea, '\x1122334455667788'::bytea);

-- Schema posture: all three vault tables carry FORCE row security; runtime is NOBYPASSRLS.
DO $$
BEGIN
  IF (SELECT rolbypassrls FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE EXCEPTION 'P10 FAIL: aimee_kb_runtime has BYPASSRLS';
  END IF;
  IF (SELECT relforcerowsecurity FROM pg_class WHERE relname = 'org_vault_secret') IS NOT TRUE THEN
    RAISE EXCEPTION 'P10 FAIL: org_vault_secret is not FORCE ROW LEVEL SECURITY';
  END IF;
  IF (SELECT relforcerowsecurity FROM pg_class WHERE relname = 'org_vault_current') IS NOT TRUE THEN
    RAISE EXCEPTION 'P10 FAIL: org_vault_current is not FORCE ROW LEVEL SECURITY';
  END IF;
  IF (SELECT relforcerowsecurity FROM pg_class WHERE relname = 'org_vault_salt') IS NOT TRUE THEN
    RAISE EXCEPTION 'P10 FAIL: org_vault_salt is not FORCE ROW LEVEL SECURITY';
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- Drop to the runtime role; RLS now governs every read.
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;

-- member_a's request: enter alpha's tenant context.
SELECT set_tenant_context('oidc:test:member_a', 940001);

DO $$
DECLARE own_n int; other_n int; plat_n int; total_n int;
BEGIN
  SELECT count(*) INTO own_n   FROM org_vault_secret WHERE team_id = 940001;
  SELECT count(*) INTO other_n FROM org_vault_secret WHERE team_id = 940002;
  SELECT count(*) INTO plat_n  FROM org_vault_secret WHERE team_id IS NULL;
  SELECT count(*) INTO total_n FROM org_vault_secret;
  IF own_n <> 1 OR other_n <> 0 THEN
    RAISE EXCEPTION 'P10 FAIL: team read isolation own=% other=% (want 1/0)', own_n, other_n;
  END IF;
  -- THE CRITICAL v2 INVARIANT: platform-scoped rows are NEVER a tenant read.
  IF plat_n <> 0 THEN
    RAISE EXCEPTION 'P10 FAIL: member_a saw % platform-scoped (team_id IS NULL) rows (want 0)', plat_n;
  END IF;
  IF total_n <> 1 THEN
    RAISE EXCEPTION 'P10 FAIL: member_a total visible = % (want 1: only its own team row)', total_n;
  END IF;
END $$;

-- Privilege floor: the runtime role cannot forge a vault row directly (writes must go
-- through the SECURITY DEFINER vault functions).
DO $$
BEGIN
  BEGIN
    INSERT INTO org_vault_secret(principal, team_id, agent, cred, version, wrapped_dek, nonce, ciphertext, tag)
      VALUES ('team:940001:provider:forge', 940001, 'x', 'y', 1, '\x00'::bytea, '\x00'::bytea, '\x00'::bytea, '\x00'::bytea);
    RAISE EXCEPTION 'P10 FAIL: runtime forged a direct org_vault_secret INSERT';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: no INSERT grant
  END;
END $$;

-- Symmetric isolation: member_b sees only beta, and no platform row.
SELECT set_tenant_context('oidc:test:member_b', 940002);
DO $$
DECLARE a_n int; b_n int; plat_n int;
BEGIN
  SELECT count(*) INTO a_n    FROM org_vault_secret WHERE team_id = 940001;
  SELECT count(*) INTO b_n    FROM org_vault_secret WHERE team_id = 940002;
  SELECT count(*) INTO plat_n FROM org_vault_secret WHERE team_id IS NULL;
  IF a_n <> 0 OR b_n <> 1 OR plat_n <> 0 THEN
    RAISE EXCEPTION 'P10 FAIL: member_b sees alpha=% beta=% platform=% (want 0/1/0)', a_n, b_n, plat_n;
  END IF;
END $$;

-- An org-admin (principal 'owner' => kb_principal_is_admin()) DOES see the platform row
-- and every team row — the ONLY read path to a team_id IS NULL row.
SELECT set_tenant_context('owner', 0);
DO $$
DECLARE plat_n int; total_n int;
BEGIN
  SELECT count(*) INTO plat_n  FROM org_vault_secret WHERE team_id IS NULL;
  SELECT count(*) INTO total_n FROM org_vault_secret;
  IF plat_n <> 1 THEN
    RAISE EXCEPTION 'P10 FAIL: admin sees % platform rows (want 1 — the CA key)', plat_n;
  END IF;
  IF total_n <> 3 THEN
    RAISE EXCEPTION 'P10 FAIL: admin total visible = % (want 3: both teams + platform)', total_n;
  END IF;
END $$;

RESET ROLE;

-- ----------------------------------------------------------------------------
-- Correctness of the version-pointer discipline (owner context).
-- ----------------------------------------------------------------------------
SELECT set_config('aimee.principal', 'owner', true);
DO $$
DECLARE v bigint;
BEGIN
  -- The seeded platform slot is at version 1.
  IF org_vault_has('org:pki:ca-key', 'kb', 'ca_key') <> 1 THEN
    RAISE EXCEPTION 'P10 FAIL: initial version pointer <> 1';
  END IF;
  -- A second put advances the pointer to 2 (monotonic, immutable prior row retained).
  v := org_vault_put('org:pki:ca-key', NULL, 'kb', 'ca_key', 2,
    '\xCAFEBABE'::bytea, '\x0102030405060708090A0B0C'::bytea, '\xAABBCC'::bytea, '\x1122334455667788'::bytea);
  IF v <> 2 THEN RAISE EXCEPTION 'P10 FAIL: second put returned version % (want 2)', v; END IF;
  IF org_vault_has('org:pki:ca-key', 'kb', 'ca_key') <> 2 THEN
    RAISE EXCEPTION 'P10 FAIL: current pointer did not advance to 2';
  END IF;
  -- Both immutable version rows are retained.
  IF (SELECT count(*) FROM org_vault_secret WHERE principal = 'org:pki:ca-key') <> 2 THEN
    RAISE EXCEPTION 'P10 FAIL: expected 2 immutable version rows for the CA slot';
  END IF;
  -- A stale expected_version is refused (fail-closed anti-rollback for the AAD binding).
  BEGIN
    PERFORM org_vault_put('org:pki:ca-key', NULL, 'kb', 'ca_key', 2,
      '\x00'::bytea, '\x00'::bytea, '\x00'::bytea, '\x00'::bytea);
    RAISE EXCEPTION 'P10 FAIL: org_vault_put accepted a stale expected_version';
  EXCEPTION WHEN serialization_failure THEN NULL;  -- expected (errcode 40001)
  END;
END $$;

\echo '== P10 kb-vault RLS isolation + version-pointer assertions PASSED =='
ROLLBACK;
