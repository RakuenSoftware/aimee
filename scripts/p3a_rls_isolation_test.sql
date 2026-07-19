-- p3a_rls_isolation_test.sql: the mandatory DB-layer cost-attribution isolation
-- gate (P3a). Run against a Postgres that has had schema_roles.sql + schema.sql +
-- schema_grants.sql applied. Proves, at the DB layer under the non-owner
-- NOBYPASSRLS runtime role:
--   * cross-team ledger + rollup read isolation (a team-lead reads only its team),
--   * pricing is admin-only (team-leads/members never read raw prices) yet cost is
--     reachable through the SECURITY DEFINER estimate function (cost, not prices),
--   * the runtime role cannot forge/mutate cost rows out of band (writes go only
--     through the metering functions),
--   * WORM: pricing rows are immutable, the ledger is append + single-settle.
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p3a_rls_isolation_test.sql
--
-- Uses SET ROLE aimee_kb_runtime so a single superuser session exercises the
-- runtime role's RLS view, exactly as a runtime connection would. The whole test
-- is one transaction (matching a real request boundary, so set_tenant_context's
-- transaction-local principal GUC persists across statements) and is rolled back.

\set ON_ERROR_STOP on

BEGIN;

-- Seed as the owner principal (admin gate for the metering functions). set_config
-- is transaction-local; it persists for this single-txn test.
SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (930001, 'p3a_alpha'), (930002, 'p3a_beta');
-- lead_a leads alpha, lead_b leads beta. Leads are also members (set_tenant_context
-- validates (principal, team) membership before granting the tenant context).
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:lead_a', 930001, 1), ('oidc:test:lead_b', 930002, 1);
INSERT INTO kb_team_lead(identity_key, team)
  VALUES ('oidc:test:lead_a', 930001), ('oidc:test:lead_b', 930002);
INSERT INTO kb_project(id, parent, name) VALUES (930010, 930001, 'alpha_proj');

-- Pricing + two ledger rows (one per team) via the metering functions.
SELECT org_pricing_add_version('anthropic', 'p3a-model', 10, 20, 1, 2);
SELECT org_token_audit_start('rA', 'origin-A', '', '', 930001, 930010, 'p3a-model', 1, '', '');
SELECT org_token_audit_settle('origin-A', 'rA', 'settled_success', 'p3a-model', 100, 50, 0, 0, 0.002, '2026-07-19');
SELECT org_token_audit_start('rB', 'origin-B', '', '', 930002, NULL, 'p3a-model', 1, '', '');
SELECT org_token_audit_settle('origin-B', 'rB', 'settled_success', 'p3a-model', 100, 50, 0, 0, 0.002, '2026-07-19');

-- Schema posture: the org cost tables carry row security; kb_team_lead is FORCEd
-- (human-admin-written, P1 pattern); runtime is NOBYPASSRLS.
DO $$
BEGIN
  IF (SELECT rolbypassrls FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE EXCEPTION 'P3a FAIL: aimee_kb_runtime has BYPASSRLS';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_token_audit') IS NOT TRUE THEN
    RAISE EXCEPTION 'P3a FAIL: org_token_audit has no row security';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_spend_rollup') IS NOT TRUE THEN
    RAISE EXCEPTION 'P3a FAIL: org_spend_rollup has no row security';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_model_pricing') IS NOT TRUE THEN
    RAISE EXCEPTION 'P3a FAIL: org_model_pricing has no row security';
  END IF;
  IF (SELECT relforcerowsecurity FROM pg_class WHERE relname = 'kb_team_lead') IS NOT TRUE THEN
    RAISE EXCEPTION 'P3a FAIL: kb_team_lead is not FORCE ROW LEVEL SECURITY';
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- Drop to the runtime role; RLS now governs every read.
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;

-- lead_a's request: enter alpha's tenant context.
SELECT set_tenant_context('oidc:test:lead_a', 930001);

DO $$
DECLARE own_n int; other_n int; total_n int; roll_own int; roll_other int; price_n int; cost numeric;
BEGIN
  SELECT count(*) INTO own_n   FROM org_token_audit WHERE team_id = 930001;
  SELECT count(*) INTO other_n FROM org_token_audit WHERE team_id = 930002;
  SELECT count(*) INTO total_n FROM org_token_audit;
  IF own_n <> 1 OR other_n <> 0 OR total_n <> 1 THEN
    RAISE EXCEPTION 'P3a FAIL: ledger isolation own=% other=% total=% (want 1/0/1)', own_n, other_n, total_n;
  END IF;

  SELECT count(*) INTO roll_own   FROM org_spend_rollup WHERE team_id = 930001;
  SELECT count(*) INTO roll_other FROM org_spend_rollup WHERE team_id = 930002;
  IF roll_own <> 1 OR roll_other <> 0 THEN
    RAISE EXCEPTION 'P3a FAIL: rollup isolation own=% other=% (want 1/0)', roll_own, roll_other;
  END IF;

  -- Pricing is admin-only: lead_a (non-admin) must see zero raw price rows...
  SELECT count(*) INTO price_n FROM org_model_pricing;
  IF price_n <> 0 THEN
    RAISE EXCEPTION 'P3a FAIL: non-admin lead read % price rows (want 0)', price_n;
  END IF;
  -- ...yet the cost is reachable via the SECURITY DEFINER estimate (cost only).
  SELECT org_token_estimate_cost('p3a-model', 1, 100, 50, 0, 0) INTO cost;
  IF cost IS NULL OR cost <= 0 THEN
    RAISE EXCEPTION 'P3a FAIL: cost estimate unreachable for non-admin (got %)', cost;
  END IF;
END $$;

-- Privilege floor: the runtime role cannot forge or mutate a ledger row directly
-- (all writes must go through the metering functions).
DO $$
BEGIN
  BEGIN
    INSERT INTO org_token_audit(request_id, origin_cert_cn, team_id, billable_model, pricing_version)
      VALUES ('forge', 'origin-F', 930001, 'p3a-model', 1);
    RAISE EXCEPTION 'P3a FAIL: runtime forged a direct ledger INSERT';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: no INSERT grant
  END;
  BEGIN
    UPDATE org_model_pricing SET input_usd_per_mtok = 0 WHERE billable_model = 'p3a-model';
    RAISE EXCEPTION 'P3a FAIL: runtime mutated a price row';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: no UPDATE grant
  END;
END $$;

-- Symmetric isolation: lead_b sees only beta.
SELECT set_tenant_context('oidc:test:lead_b', 930002);
DO $$
DECLARE a_n int; b_n int;
BEGIN
  SELECT count(*) INTO a_n FROM org_token_audit WHERE team_id = 930001;
  SELECT count(*) INTO b_n FROM org_token_audit WHERE team_id = 930002;
  IF a_n <> 0 OR b_n <> 1 THEN
    RAISE EXCEPTION 'P3a FAIL: lead_b sees alpha=% beta=% (want 0/1)', a_n, b_n;
  END IF;
END $$;

RESET ROLE;

-- WORM at the owner level (owner bypasses ENABLE-not-FORCE RLS, so these failures
-- come purely from the triggers, proving immutability is trigger-enforced not just
-- grant-gated). Pricing UPDATE/DELETE and ledger DELETE must all raise.
DO $$
BEGIN
  BEGIN
    UPDATE org_model_pricing SET output_usd_per_mtok = 1 WHERE billable_model = 'p3a-model' AND version = 1;
    RAISE EXCEPTION 'P3a FAIL: pricing UPDATE was not blocked by WORM trigger';
  EXCEPTION WHEN sqlstate '42501' THEN NULL;
  END;
  BEGIN
    DELETE FROM org_model_pricing WHERE billable_model = 'p3a-model';
    RAISE EXCEPTION 'P3a FAIL: pricing DELETE was not blocked by WORM trigger';
  EXCEPTION WHEN sqlstate '42501' THEN NULL;
  END;
  BEGIN
    DELETE FROM org_token_audit WHERE request_id = 'rA';
    RAISE EXCEPTION 'P3a FAIL: ledger DELETE was not blocked by WORM trigger';
  EXCEPTION WHEN sqlstate '42501' THEN NULL;
  END;
END $$;

-- ----------------------------------------------------------------------------
-- Core correctness of the metering functions (owner context; the acceptance
-- claims the plan makes, proven in CI rather than only in scratch).
-- ----------------------------------------------------------------------------
SELECT set_config('aimee.principal', 'owner', true);

-- Idempotent start + replay-mismatch rejection.
DO $$
DECLARE id1 bigint; id2 bigint;
BEGIN
  id1 := org_token_audit_start('cr', 'origin-C', 'iss', 'sub', 930001, 930010, 'p3a-model', 1, '', '');
  id2 := org_token_audit_start('cr', 'origin-C', 'iss', 'sub', 930001, 930010, 'p3a-model', 1, '', '');
  IF id1 <> id2 THEN
    RAISE EXCEPTION 'P3a FAIL: idempotent re-start returned different ids % vs %', id1, id2;
  END IF;
  BEGIN
    -- same key, mismatched immutable triple (different team) -> reject
    PERFORM org_token_audit_start('cr', 'origin-C', 'iss', 'sub', 930002, NULL, 'p3a-model', 1, '', '');
    RAISE EXCEPTION 'P3a FAIL: replay with mismatched attributes was accepted';
  EXCEPTION WHEN unique_violation THEN NULL;  -- expected (errcode 23505)
  END;
  -- same request_id from a DIFFERENT origin is a distinct valid row, not a replay
  IF org_token_audit_start('cr', 'origin-D', '', '', 930001, 930010, 'p3a-model', 1, '', '')
       = id1 THEN
    RAISE EXCEPTION 'P3a FAIL: cross-origin same request_id collapsed onto the same row';
  END IF;
END $$;

-- Double-settle is a no-op (state-checked UPDATE); rollup increments exactly once.
DO $$
DECLARE ok boolean; rc bigint;
BEGIN
  ok := org_token_audit_settle('origin-C', 'cr', 'settled_success', 'p3a-model', 10, 5, 0, 0, 0.0002, '2026-07-20');
  IF NOT ok THEN RAISE EXCEPTION 'P3a FAIL: first settle returned false'; END IF;
  ok := org_token_audit_settle('origin-C', 'cr', 'settled_failed', 'x', 99, 99, 0, 0, 9.9, '2026-07-20');
  IF ok THEN RAISE EXCEPTION 'P3a FAIL: double-settle of terminal row returned true'; END IF;
  SELECT row_count INTO rc FROM org_spend_rollup
    WHERE team_id = 930001 AND billable_model = 'p3a-model' AND day = '2026-07-20';
  IF rc <> 1 THEN RAISE EXCEPTION 'P3a FAIL: rollup row_count=% after double-settle (want 1)', rc; END IF;
END $$;

-- indeterminate -> settled reconciliation contributes to the rollup exactly ONCE.
DO $$
DECLARE rc bigint; pt bigint;
BEGIN
  PERFORM org_token_audit_start('ir', 'origin-C', '', '', 930001, 930010, 'p3a-model', 1, '', '');
  PERFORM org_token_audit_settle('origin-C', 'ir', 'indeterminate', '', 0, 0, 0, 0, 0, '2026-07-20');
  SELECT row_count INTO rc FROM org_spend_rollup
    WHERE team_id = 930001 AND billable_model = 'p3a-model' AND day = '2026-07-20';
  IF rc <> 1 THEN RAISE EXCEPTION 'P3a FAIL: indeterminate wrote a rollup delta (row_count=%, want 1)', rc; END IF;
  PERFORM org_token_audit_settle('origin-C', 'ir', 'settled_success', 'p3a-model', 20, 10, 0, 0, 0.0004, '2026-07-20');
  SELECT row_count, prompt_tokens INTO rc, pt FROM org_spend_rollup
    WHERE team_id = 930001 AND billable_model = 'p3a-model' AND day = '2026-07-20';
  IF rc <> 2 OR pt <> 30 THEN
    RAISE EXCEPTION 'P3a FAIL: reconciliation double/under-count (row_count=% prompt=%, want 2/30)', rc, pt;
  END IF;
END $$;

-- Monotonic versioning: a second price version bumps the current pointer to 2.
DO $$
DECLARE v bigint;
BEGIN
  v := org_pricing_add_version('anthropic', 'p3a-model', 11, 21, 1, 2);
  IF v <> 2 THEN RAISE EXCEPTION 'P3a FAIL: second version = % (want 2)', v; END IF;
  IF org_pricing_current_version('p3a-model') <> 2 THEN
    RAISE EXCEPTION 'P3a FAIL: current pointer did not advance to 2';
  END IF;
END $$;

\echo '== P3a RLS/cost isolation + correctness assertions PASSED =='
ROLLBACK;
