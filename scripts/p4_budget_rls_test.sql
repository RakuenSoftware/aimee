-- p4_budget_rls_test.sql: the DB-layer budget-reservation-core correctness + isolation
-- gate (P4a). Run against a Postgres that has had schema_roles.sql + schema.sql +
-- schema_grants.sql applied. Proves, single-session (the genuinely-parallel over-commit
-- race lives in scripts/p4_budget_concurrency.sh):
--   * settle reconcile: reserved down, spend up, cap-preserving LEAST(realized,max),
--     idempotent (double-settle is a no-op);
--   * an expired lease settles at reserved_max; a late reconcile adjusts spend DOWN;
--   * team + project caps are cumulative (both alloc rows; refused if EITHER exhausted);
--   * budget read RLS (a team-lead reads only its own team's caps/counters);
--   * a retroactive reduction of a limit below committed spend+reserved is rejected;
--   * a SEQUENTIAL reserve-until-refused holds spend + reserved <= limit at every step.
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p4_budget_rls_test.sql
--
-- Uses SET ROLE aimee_kb_runtime for the RLS-read assertions (a superuser session
-- exercises the non-owner runtime role's RLS view). The correctness assertions run as
-- the owner principal (the admin gate for org_budget_set). One transaction, rolled back.

\set ON_ERROR_STOP on

BEGIN;

SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (940001, 'p4_alpha'), (940002, 'p4_beta');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:p4lead_a', 940001, 1), ('oidc:test:p4lead_b', 940002, 1);
INSERT INTO kb_team_lead(identity_key, team)
  VALUES ('oidc:test:p4lead_a', 940001), ('oidc:test:p4lead_b', 940002);
INSERT INTO kb_project(id, parent, name) VALUES (940010, 940001, 'p4_alpha_proj');

-- Schema posture: budget config + counters carry row security; the runtime role is
-- NOBYPASSRLS; the expression UNIQUE INDEXes exist (no expression in a PK/UNIQUE list).
DO $$
BEGIN
  IF (SELECT rolbypassrls FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE EXCEPTION 'P4 FAIL: aimee_kb_runtime has BYPASSRLS';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_budget') IS NOT TRUE THEN
    RAISE EXCEPTION 'P4 FAIL: org_budget has no row security';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_budget_counter') IS NOT TRUE THEN
    RAISE EXCEPTION 'P4 FAIL: org_budget_counter has no row security';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_indexes WHERE indexname = 'idx_org_budget_key') THEN
    RAISE EXCEPTION 'P4 FAIL: org_budget expression UNIQUE INDEX missing';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_indexes WHERE indexname = 'idx_org_budget_counter_key') THEN
    RAISE EXCEPTION 'P4 FAIL: org_budget_counter expression UNIQUE INDEX missing';
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (a) settle reconcile + cap-preserving LEAST + idempotency.
-- ----------------------------------------------------------------------------
DO $$
DECLARE v_res TEXT; ok BOOLEAN; sp NUMERIC; rv NUMERIC;
BEGIN
  PERFORM org_budget_set(940001, NULL, 'day', 10.0, NULL);
  v_res := org_budget_reserve('origin-A', 'rq1', 940001, NULL, 1, 3.0, 3600);
  IF v_res <> 'granted' THEN RAISE EXCEPTION 'P4 FAIL: reserve rq1 = % (want granted)', v_res; END IF;
  SELECT spend_usd, reserved_usd INTO sp, rv FROM org_budget_counter
    WHERE team_id = 940001 AND COALESCE(project_id,0) = 0 AND period = 'day';
  IF sp <> 0 OR rv <> 3.0 THEN RAISE EXCEPTION 'P4 FAIL: post-reserve spend=%/reserved=% (want 0/3)', sp, rv; END IF;

  -- realized 2.0 < reserved_max 3.0 -> charge 2.0, release 3.0.
  ok := org_budget_settle('origin-A', 'rq1', 2.0);
  IF NOT ok THEN RAISE EXCEPTION 'P4 FAIL: first settle returned false'; END IF;
  SELECT spend_usd, reserved_usd INTO sp, rv FROM org_budget_counter
    WHERE team_id = 940001 AND COALESCE(project_id,0) = 0 AND period = 'day';
  IF sp <> 2.0 OR rv <> 0 THEN RAISE EXCEPTION 'P4 FAIL: post-settle spend=%/reserved=% (want 2/0)', sp, rv; END IF;

  -- Double-settle is a no-op (returns false; counter unchanged).
  ok := org_budget_settle('origin-A', 'rq1', 9.9);
  IF ok THEN RAISE EXCEPTION 'P4 FAIL: double-settle of settled row returned true'; END IF;
  SELECT spend_usd INTO sp FROM org_budget_counter
    WHERE team_id = 940001 AND COALESCE(project_id,0) = 0 AND period = 'day';
  IF sp <> 2.0 THEN RAISE EXCEPTION 'P4 FAIL: double-settle mutated spend=% (want 2)', sp; END IF;

  -- Cap-preserving clamp: realized ABOVE reserved_max charges only reserved_max.
  PERFORM org_budget_reserve('origin-A', 'rq2', 940001, NULL, 1, 4.0, 3600);
  PERFORM org_budget_settle('origin-A', 'rq2', 100.0);  -- realized >> reserved_max 4.0
  SELECT spend_usd, reserved_usd INTO sp, rv FROM org_budget_counter
    WHERE team_id = 940001 AND COALESCE(project_id,0) = 0 AND period = 'day';
  IF sp <> 6.0 OR rv <> 0 THEN RAISE EXCEPTION 'P4 FAIL: clamp spend=%/reserved=% (want 6/0)', sp, rv; END IF;

  -- Idempotency read-back: identical retry returns granted; a mismatched triple rejects.
  v_res := org_budget_reserve('origin-A', 'rq2', 940001, NULL, 1, 4.0, 3600);
  IF v_res <> 'granted' THEN RAISE EXCEPTION 'P4 FAIL: idempotent re-reserve = % (want granted)', v_res; END IF;
  BEGIN
    PERFORM org_budget_reserve('origin-A', 'rq2', 940001, NULL, 1, 9.0, 3600);  -- different reserved_max
    RAISE EXCEPTION 'P4 FAIL: mismatched replay was accepted';
  EXCEPTION WHEN unique_violation THEN NULL;  -- expected 23505
  END;
END $$;

-- ----------------------------------------------------------------------------
-- (b) an expired lease settles at reserved_max; a late reconcile adjusts DOWN only.
-- ----------------------------------------------------------------------------
DO $$
DECLARE n INT; sp NUMERIC; rv NUMERIC; st TEXT;
BEGIN
  PERFORM org_budget_set(940002, NULL, 'day', 20.0, NULL);
  -- 1-second lease, then backdate it so the sweeper sees it lapsed.
  PERFORM org_budget_reserve('origin-B', 'exp1', 940002, NULL, 1, 5.0, 1);
  UPDATE org_budget_reservation SET lease_expires_at = now() - interval '1 hour'
    WHERE origin_cert_cn = 'origin-B' AND request_id = 'exp1';
  n := org_budget_settle_expired();
  IF n < 1 THEN RAISE EXCEPTION 'P4 FAIL: settle_expired swept % (want >=1)', n; END IF;
  SELECT spend_usd, reserved_usd INTO sp, rv FROM org_budget_counter
    WHERE team_id = 940002 AND COALESCE(project_id,0) = 0 AND period = 'day';
  IF sp <> 5.0 OR rv <> 0 THEN RAISE EXCEPTION 'P4 FAIL: expired charge spend=%/reserved=% (want 5/0)', sp, rv; END IF;
  SELECT state INTO st FROM org_budget_reservation WHERE origin_cert_cn = 'origin-B' AND request_id = 'exp1';
  IF st <> 'expired_settled' THEN RAISE EXCEPTION 'P4 FAIL: expired state=% (want expired_settled)', st; END IF;

  -- Late reconcile with realized 1.0 < 5.0 charged -> spend adjusts DOWN to 1.0.
  PERFORM org_budget_settle('origin-B', 'exp1', 1.0);
  SELECT spend_usd INTO sp FROM org_budget_counter
    WHERE team_id = 940002 AND COALESCE(project_id,0) = 0 AND period = 'day';
  IF sp <> 1.0 THEN RAISE EXCEPTION 'P4 FAIL: late downward adjust spend=% (want 1)', sp; END IF;

  -- A late reconcile that would raise spend is a no-op (never adjusts UP).
  PERFORM org_budget_settle('origin-B', 'exp1', 4.0);
  SELECT spend_usd INTO sp FROM org_budget_counter
    WHERE team_id = 940002 AND COALESCE(project_id,0) = 0 AND period = 'day';
  IF sp <> 1.0 THEN RAISE EXCEPTION 'P4 FAIL: late reconcile raised spend to % (want 1)', sp; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (c) team + project caps cumulative: BOTH alloc rows; refused if EITHER exhausted.
-- ----------------------------------------------------------------------------
DO $$
DECLARE v_res TEXT; n INT;
BEGIN
  -- team-day cap 50, project-day cap 4. A 3-unit reserve fits both -> 2 alloc rows.
  PERFORM org_budget_set(940001, NULL, 'day', 50.0, NULL);          -- (already set to 10 above; raise it)
  PERFORM org_budget_set(940001, 940010, 'day', 4.0, NULL);
  v_res := org_budget_reserve('origin-C', 'cum1', 940001, 940010, 1, 3.0, 3600);
  IF v_res <> 'granted' THEN RAISE EXCEPTION 'P4 FAIL: cumulative reserve = % (want granted)', v_res; END IF;
  SELECT count(*) INTO n FROM org_budget_reservation_alloc a
    JOIN org_budget_reservation r ON r.id = a.reservation_id
    WHERE r.origin_cert_cn = 'origin-C' AND r.request_id = 'cum1';
  IF n <> 2 THEN RAISE EXCEPTION 'P4 FAIL: cumulative reserve bound % counters (want 2)', n; END IF;

  -- A second 3-unit reserve: team has room (50) but project (4) has only 1 left -> refused
  -- on the PROJECT scope, and NOTHING is reserved (team counter unchanged from the refusal).
  v_res := org_budget_reserve('origin-C', 'cum2', 940001, 940010, 1, 3.0, 3600);
  IF v_res <> 'refused:project budget exceeded' THEN
    RAISE EXCEPTION 'P4 FAIL: over-project reserve = % (want refused:project budget exceeded)', v_res;
  END IF;
  -- The refusal persisted no reservation and no counter change beyond cum1's 3.0.
  IF EXISTS (SELECT 1 FROM org_budget_reservation WHERE origin_cert_cn='origin-C' AND request_id='cum2') THEN
    RAISE EXCEPTION 'P4 FAIL: refused reserve persisted a reservation row';
  END IF;
  PERFORM 1 FROM org_budget_counter WHERE team_id=940001 AND COALESCE(project_id,0)=0 AND period='day'
    AND reserved_usd = 3.0;
  IF NOT FOUND THEN RAISE EXCEPTION 'P4 FAIL: team counter reserved != 3.0 after refusal (partial reserve leaked)'; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (e) retroactive-reduction reject: lower a limit below current committed -> RAISE.
-- ----------------------------------------------------------------------------
DO $$
BEGIN
  -- team 940001 day has 6.0 spend (from (a)) + 3.0 reserved (from (c)) = 9.0 committed.
  BEGIN
    PERFORM org_budget_set(940001, NULL, 'day', 5.0, NULL);  -- 5 < 9 committed
    RAISE EXCEPTION 'P4 FAIL: retroactive reduction below committed was accepted';
  EXCEPTION WHEN check_violation THEN NULL;  -- expected 23514
  END;
  -- Raising the limit (or lowering only to >= committed) is allowed.
  PERFORM org_budget_set(940001, NULL, 'day', 12.0, NULL);
END $$;

-- ----------------------------------------------------------------------------
-- (f) SEQUENTIAL reserve-until-refused: spend + reserved <= limit at EVERY step.
-- ----------------------------------------------------------------------------
DO $$
DECLARE i INT; v_res TEXT; granted INT := 0; lim NUMERIC; committed NUMERIC;
BEGIN
  PERFORM org_budget_set(940002, NULL, 'month', 10.0, NULL);
  lim := 10.0;
  -- Each reserve is 2.0; the month cap admits exactly 5. Assert the invariant each step.
  FOR i IN 1..8 LOOP
    v_res := org_budget_reserve('origin-S', 'seq' || i::text, 940002, NULL, 1, 2.0, 3600);
    IF v_res = 'granted' THEN granted := granted + 1; END IF;
    SELECT (spend_usd + reserved_usd) INTO committed FROM org_budget_counter
      WHERE team_id = 940002 AND COALESCE(project_id,0) = 0 AND period = 'month';
    IF committed > lim THEN
      RAISE EXCEPTION 'P4 FAIL: committed % > limit % at step % (over-commit)', committed, lim, i;
    END IF;
  END LOOP;
  IF granted <> 5 THEN RAISE EXCEPTION 'P4 FAIL: sequential granted=% (want exactly 5)', granted; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (d) budget read RLS: a team-lead reads only its own team's caps/counters.
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:p4lead_a', 940001);
DO $$
DECLARE own_n INT; other_n INT; cnt_own INT; cnt_other INT;
BEGIN
  SELECT count(*) INTO own_n   FROM org_budget WHERE team_id = 940001;
  SELECT count(*) INTO other_n FROM org_budget WHERE team_id = 940002;
  IF own_n < 1 OR other_n <> 0 THEN
    RAISE EXCEPTION 'P4 FAIL: budget RLS lead_a own=% other=% (want >=1/0)', own_n, other_n;
  END IF;
  SELECT count(*) INTO cnt_own   FROM org_budget_counter WHERE team_id = 940001;
  SELECT count(*) INTO cnt_other FROM org_budget_counter WHERE team_id = 940002;
  IF cnt_own < 1 OR cnt_other <> 0 THEN
    RAISE EXCEPTION 'P4 FAIL: counter RLS lead_a own=% other=% (want >=1/0)', cnt_own, cnt_other;
  END IF;
  -- Reservation/alloc rows are admin-only: a non-admin lead sees none.
  IF (SELECT count(*) FROM org_budget_reservation) <> 0 THEN
    RAISE EXCEPTION 'P4 FAIL: non-admin lead read reservation rows (want 0)';
  END IF;
  -- The runtime role cannot forge a direct counter write (definer-only path).
  BEGIN
    INSERT INTO org_budget_counter(team_id, period, period_id, spend_usd)
      VALUES (940001, 'day', 'forge', 999);
    RAISE EXCEPTION 'P4 FAIL: runtime forged a direct counter INSERT';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: no INSERT grant
  END;
END $$;

-- org_budget_show is actor-bound: lead_a may read team 940001, but NOT team 940002.
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM org_budget_show(940001, NULL);
  IF n < 1 THEN RAISE EXCEPTION 'P4 FAIL: lead_a org_budget_show(own) returned % rows (want >=1)', n; END IF;
  BEGIN
    PERFORM org_budget_show(940002, NULL);
    RAISE EXCEPTION 'P4 FAIL: lead_a read team 940002 budget (cross-team)';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected 42501
  END;
END $$;
RESET ROLE;

\echo '== P4a budget reservation core + RLS assertions PASSED =='
ROLLBACK;
