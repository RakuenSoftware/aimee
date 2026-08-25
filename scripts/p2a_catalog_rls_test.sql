-- p2a_catalog_rls_test.sql: the mandatory DB-layer org-model catalog + entitlement
-- isolation gate (P2a). Run against a Postgres that has had schema_roles.sql +
-- schema.sql + schema_grants.sql applied. Proves, at the DB layer under the non-owner
-- NOBYPASSRLS runtime role:
--   (a) cross-team entitlement isolation — org_catalog_entitled() for an actor in team A
--       returns ONLY A's entitled models, never team B's;
--   (b) org_catalog_entitled() EXCLUDES enabled=false catalog rows;
--   (c) the runtime role has NO direct SELECT on org_model_catalog (privilege denied) —
--       every catalog read must funnel through the definer function;
--   (d) a WORM kb_audit_outbox row is appended atomically on a catalog mutation;
--   (e) a non-admin principal cannot mutate the catalog (admin gate raises).
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p2a_catalog_rls_test.sql
--
-- Uses SET ROLE aimee_kb_runtime so a single superuser session exercises the runtime
-- role's RLS view. The whole test is one transaction (so set_tenant_context's
-- transaction-local principal GUC persists across statements) and is rolled back.

\set ON_ERROR_STOP on

BEGIN;

-- Seed as the owner principal. The catalog definer functions run as the (superuser)
-- owner and bypass RLS for their internal writes, exactly like P3a/P10.
SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (950001, 'p2a_alpha'), (950002, 'p2a_beta');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:p2a_member_a', 950001, 1), ('oidc:test:p2a_member_b', 950002, 1);

-- Catalog entries via the audited definer (owner => kb_principal_is_admin()). One
-- disabled row proves the enabled-filter; endpoint is catalog-owned (may be empty).
SELECT org_catalog_upsert('p2a-anthropic', 'Claude',  'anthropic', 'anthropic', '',                       true);
SELECT org_catalog_upsert('p2a-openai',    'GPT',     'openai',    'openai',    'https://api.openai.test', true);
SELECT org_catalog_upsert('p2a-disabled',  'Retired', 'anthropic', 'anthropic', '',                       false);

-- Entitlements: alpha gets anthropic + the disabled model; beta gets openai.
SELECT org_model_entitle('p2a-anthropic', 950001);
SELECT org_model_entitle('p2a-disabled',  950001);
SELECT org_model_entitle('p2a-openai',    950002);

-- Schema posture: both P2a tables carry ENABLE row security; runtime is NOBYPASSRLS.
DO $$
BEGIN
  IF (SELECT rolbypassrls FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE EXCEPTION 'P2a FAIL: aimee_kb_runtime has BYPASSRLS';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_model_catalog') IS NOT TRUE THEN
    RAISE EXCEPTION 'P2a FAIL: org_model_catalog does not have ROW LEVEL SECURITY enabled';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_model_entitlement') IS NOT TRUE THEN
    RAISE EXCEPTION 'P2a FAIL: org_model_entitlement does not have ROW LEVEL SECURITY enabled';
  END IF;
END $$;

-- (d) A WORM kb_audit_outbox row is appended atomically on a catalog mutation. Capture
-- the count, do one more upsert, and assert exactly one new row with the right action.
DO $$
DECLARE n0 bigint; n1 bigint; last_action text;
BEGIN
  SELECT count(*) INTO n0 FROM kb_audit_outbox;
  PERFORM org_catalog_upsert('p2a-audited', 'Audited', 'gemini', 'gemini', '', true);
  SELECT count(*) INTO n1 FROM kb_audit_outbox;
  IF n1 <> n0 + 1 THEN
    RAISE EXCEPTION 'P2a FAIL: catalog upsert did not append exactly one audit row (% -> %)', n0, n1;
  END IF;
  SELECT action INTO last_action FROM kb_audit_outbox ORDER BY outbox_id DESC LIMIT 1;
  IF last_action <> 'org_catalog_upsert' THEN
    RAISE EXCEPTION 'P2a FAIL: newest audit action = % (want org_catalog_upsert)', last_action;
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- Drop to the runtime role; RLS + grants now govern every access.
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;

-- (c) The runtime role has NO direct SELECT on org_model_catalog — every catalog read
-- must go through org_catalog_entitled().
DO $$
BEGIN
  BEGIN
    PERFORM 1 FROM org_model_catalog LIMIT 1;
    RAISE EXCEPTION 'P2a FAIL: runtime performed a direct SELECT on org_model_catalog';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: no SELECT grant
  END;
END $$;

-- (a)+(b) member_a's request: enter alpha's tenant context; org_catalog_entitled() is
-- actor-bound to aimee.principal, so it returns ONLY alpha's ENABLED entitled models.
SELECT set_tenant_context('oidc:test:p2a_member_a', 950001);
DO $$
DECLARE total int; anth int; openai_n int; disabled_n int; cred_cols int;
BEGIN
  SELECT count(*) INTO total FROM org_catalog_entitled();
  SELECT count(*) INTO anth       FROM org_catalog_entitled() WHERE model_id = 'p2a-anthropic';
  SELECT count(*) INTO openai_n   FROM org_catalog_entitled() WHERE model_id = 'p2a-openai';
  SELECT count(*) INTO disabled_n FROM org_catalog_entitled() WHERE model_id = 'p2a-disabled';
  IF anth <> 1 THEN
    RAISE EXCEPTION 'P2a FAIL: member_a missing its entitled anthropic model (got %)', anth;
  END IF;
  -- (a) cross-team isolation: beta's model is NEVER visible to alpha's member.
  IF openai_n <> 0 THEN
    RAISE EXCEPTION 'P2a FAIL: member_a saw team B''s entitled model (cross-team leak)';
  END IF;
  -- (b) the disabled catalog row is excluded even though alpha is entitled to it.
  IF disabled_n <> 0 THEN
    RAISE EXCEPTION 'P2a FAIL: member_a saw a disabled (enabled=false) model';
  END IF;
  IF total <> 1 THEN
    RAISE EXCEPTION 'P2a FAIL: member_a total entitled = % (want 1: only the enabled alpha model)', total;
  END IF;
  -- The entitled surface exposes ONLY the authoritative catalog columns — no credential
  -- / slot field ever leaks (there is no such column in the result type).
  SELECT count(*) INTO cred_cols
    FROM information_schema.routines r
    JOIN information_schema.parameters p ON p.specific_name = r.specific_name
   WHERE r.routine_name = 'org_catalog_entitled'
     AND p.parameter_mode = 'OUT'
     AND (lower(p.parameter_name) LIKE '%cred%' OR lower(p.parameter_name) LIKE '%slot%'
          OR lower(p.parameter_name) LIKE '%key%' OR lower(p.parameter_name) LIKE '%secret%');
  IF cred_cols <> 0 THEN
    RAISE EXCEPTION 'P2a FAIL: org_catalog_entitled exposes a credential-like column';
  END IF;
END $$;

-- Symmetric isolation: member_b sees only beta's openai model, not alpha's.
SELECT set_tenant_context('oidc:test:p2a_member_b', 950002);
DO $$
DECLARE total int; openai_n int; anth int;
BEGIN
  SELECT count(*) INTO total    FROM org_catalog_entitled();
  SELECT count(*) INTO openai_n FROM org_catalog_entitled() WHERE model_id = 'p2a-openai';
  SELECT count(*) INTO anth     FROM org_catalog_entitled() WHERE model_id = 'p2a-anthropic';
  IF openai_n <> 1 OR anth <> 0 OR total <> 1 THEN
    RAISE EXCEPTION 'P2a FAIL: member_b entitled openai=% anthropic=% total=% (want 1/0/1)',
      openai_n, anth, total;
  END IF;
END $$;

-- (e) A non-admin principal cannot mutate the catalog: the admin gate raises.
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_upsert('p2a-evil', 'Evil', 'openai', 'openai', '', true);
    RAISE EXCEPTION 'P2a FAIL: a non-admin principal mutated the catalog';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: admin-only (errcode 42501)
  END;
END $$;

RESET ROLE;

\echo '== P2a catalog + entitlement RLS isolation + WORM-audit assertions PASSED =='
ROLLBACK;
