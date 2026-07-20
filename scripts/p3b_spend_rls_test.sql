-- p3b_spend_rls_test.sql: the mandatory DB-layer spend-reporting authorization gate
-- (P3b). Run against a Postgres that has had schema_roles.sql + schema.sql +
-- schema_grants.sql applied. Proves, at the DB layer under the non-owner NOBYPASSRLS
-- runtime role, that the SECURITY DEFINER org_spend_query() is itself the authz gate:
--   * a team-lead sees ONLY their own team's spend (cross-team query RAISEs 42501) —
--     and since the definer BYPASSES RLS to aggregate, its INTERNAL admin/lead predicate
--     is the ONLY authz on that path, so it is tested directly (the point of P3b);
--   * an org-admin (owner) org_spend_query(NULL, ...) sees ALL teams;
--   * a non-admin non-lead member RAISEs 42501 (membership alone is not lead);
--   * per-model costs reconcile to the team total (sum(by_model) == total);
--   * a date-range filter excludes out-of-window rows;
--   * a malformed date RAISEs (fail-closed date validation).
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p3b_spend_rls_test.sql
--
-- One transaction (a real request boundary; set_tenant_context's txn-local principal
-- GUC persists across statements) and rolled back.

\set ON_ERROR_STOP on

BEGIN;

-- Seed as the owner principal (admin; owner bypasses ENABLE-not-FORCE RLS to seed the
-- rollup directly — the write path proper is the P3a settle txn, not under test here).
SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (940001, 'p3b_alpha'), (940002, 'p3b_beta');
-- lead_a leads alpha, lead_b leads beta; member_c is a plain member of alpha (NOT a
-- lead). Leads/members must be members so set_tenant_context grants the tenant context.
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:lead_a', 940001, 1), ('oidc:test:lead_b', 940002, 1),
         ('oidc:test:member_c', 940001, 1);
INSERT INTO kb_team_lead(identity_key, team)
  VALUES ('oidc:test:lead_a', 940001), ('oidc:test:lead_b', 940002);
INSERT INTO kb_project(id, parent, name) VALUES (940010, 940001, 'alpha_proj');

-- Seed the rollup: 2 teams x >=2 models x >=2 days.
INSERT INTO org_spend_rollup(team_id, project_id, billable_model, day,
    prompt_tokens, completion_tokens, cache_read_tokens, cache_write_tokens, cost_usd, row_count)
  VALUES
    (940001, 940010, 'modelX', '2026-07-01', 100, 50, 0, 0, 0.0010000000, 1),
    (940001, NULL,   'modelY', '2026-07-02', 200, 80, 0, 0, 0.0020000000, 2),
    (940002, NULL,   'modelX', '2026-07-01', 300, 90, 0, 0, 0.0030000000, 3),
    (940002, NULL,   'modelY', '2026-07-03', 400, 10, 0, 0, 0.0040000000, 4);

-- ----------------------------------------------------------------------------
-- Drop to the runtime role; the definer's INTERNAL predicate is now the only gate
-- (RLS is bypassed inside the SECURITY DEFINER).
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;

-- lead_a's request: enter alpha's tenant context.
SELECT set_tenant_context('oidc:test:lead_a', 940001);

-- (i) lead_a's own-team query returns ONLY team A's rows; (v) per-model reconciliation.
DO $$
DECLARE nrows int; total numeric; by_model_sum numeric;
BEGIN
  SELECT count(*), COALESCE(sum(cost_usd), 0)
    INTO nrows, total
    FROM org_spend_query(940001, NULL, '2026-07-01', '2026-07-31');
  IF nrows <> 2 THEN
    RAISE EXCEPTION 'P3b FAIL: lead_a own-team rows=% (want 2)', nrows;
  END IF;
  IF total <> 0.0030000000 THEN
    RAISE EXCEPTION 'P3b FAIL: lead_a team total=% (want 0.003)', total;
  END IF;
  -- Reconciliation: the per-model breakdown sums back to the team total exactly.
  SELECT sum(m.c) INTO by_model_sum FROM (
    SELECT billable_model, sum(cost_usd) AS c
      FROM org_spend_query(940001, NULL, '2026-07-01', '2026-07-31')
     GROUP BY billable_model) m;
  IF by_model_sum <> total THEN
    RAISE EXCEPTION 'P3b FAIL: sum(by_model)=% <> total=%', by_model_sum, total;
  END IF;
END $$;

-- (ii) lead_a querying team B RAISEs insufficient_privilege — PROVING the function is
-- the gate (the definer bypassed RLS, so only its internal predicate denied this).
DO $$
BEGIN
  PERFORM * FROM org_spend_query(940002, NULL, '2026-07-01', '2026-07-31');
  RAISE EXCEPTION 'P3b FAIL: lead_a read team B (cross-team not denied)';
EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected (errcode 42501)
END $$;

-- lead_a is NOT an org-admin: the org-wide (NULL team) report must also be denied.
DO $$
BEGIN
  PERFORM * FROM org_spend_query(NULL, NULL, '2026-07-01', '2026-07-31');
  RAISE EXCEPTION 'P3b FAIL: non-admin lead read the org-wide report';
EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected (errcode 42501)
END $$;

-- (vi) date-range filter excludes out-of-window rows: the 07-01 row drops out.
DO $$
DECLARE nrows int; total numeric;
BEGIN
  SELECT count(*), COALESCE(sum(cost_usd), 0)
    INTO nrows, total
    FROM org_spend_query(940001, NULL, '2026-07-02', '2026-07-31');
  IF nrows <> 1 OR total <> 0.0020000000 THEN
    RAISE EXCEPTION 'P3b FAIL: date-window rows=% total=% (want 1 / 0.002)', nrows, total;
  END IF;
END $$;

-- (vii) malformed / invalid / inverted dates all RAISE (fail-closed validation).
DO $$
BEGIN
  BEGIN
    PERFORM * FROM org_spend_query(940001, NULL, '2026-13-40', '2026-07-31');
    RAISE EXCEPTION 'P3b FAIL: invalid calendar date was accepted';
  EXCEPTION WHEN sqlstate '22007' THEN NULL;  -- expected: bad date value
  END;
  BEGIN
    PERFORM * FROM org_spend_query(940001, NULL, 'not-a-date', '2026-07-31');
    RAISE EXCEPTION 'P3b FAIL: malformed date was accepted';
  EXCEPTION WHEN sqlstate '22007' THEN NULL;  -- expected: bad date format
  END;
  BEGIN
    PERFORM * FROM org_spend_query(940001, NULL, '2026-07-31', '2026-07-01');
    RAISE EXCEPTION 'P3b FAIL: inverted range (since > until) was accepted';
  EXCEPTION WHEN sqlstate '22007' THEN NULL;  -- expected: bad date range
  END;
END $$;

-- (iv) a plain member (not a lead) of alpha is denied — membership alone is not lead.
SELECT set_tenant_context('oidc:test:member_c', 940001);
DO $$
BEGIN
  PERFORM * FROM org_spend_query(940001, NULL, '2026-07-01', '2026-07-31');
  RAISE EXCEPTION 'P3b FAIL: non-lead member read team spend';
EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected (errcode 42501)
END $$;

-- Symmetric isolation: lead_b sees only beta, and is denied alpha.
SELECT set_tenant_context('oidc:test:lead_b', 940002);
DO $$
DECLARE nrows int;
BEGIN
  SELECT count(*) INTO nrows FROM org_spend_query(940002, NULL, '2026-07-01', '2026-07-31');
  IF nrows <> 2 THEN
    RAISE EXCEPTION 'P3b FAIL: lead_b own-team rows=% (want 2)', nrows;
  END IF;
  BEGIN
    PERFORM * FROM org_spend_query(940001, NULL, '2026-07-01', '2026-07-31');
    RAISE EXCEPTION 'P3b FAIL: lead_b read team A';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected
  END;
END $$;

RESET ROLE;

-- (iii) the org-admin (owner) org_spend_query(NULL, ...) sees ALL teams' spend.
SELECT set_config('aimee.principal', 'owner', true);
DO $$
DECLARE nrows int; total numeric;
BEGIN
  SELECT count(*), COALESCE(sum(cost_usd), 0)
    INTO nrows, total
    FROM org_spend_query(NULL, NULL, '2026-07-01', '2026-07-31');
  IF nrows <> 4 THEN
    RAISE EXCEPTION 'P3b FAIL: admin org-wide rows=% (want 4)', nrows;
  END IF;
  IF total <> 0.0100000000 THEN
    RAISE EXCEPTION 'P3b FAIL: admin org-wide total=% (want 0.01)', total;
  END IF;
END $$;

\echo '== P3b spend-reporting authorization + reconciliation assertions PASSED =='
ROLLBACK;
