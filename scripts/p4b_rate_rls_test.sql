-- p4b_rate_rls_test.sql: the DB-layer keyed fixed-window rate-limiter correctness +
-- isolation gate (P4b). Run against a Postgres that has had schema_roles.sql + schema.sql
-- + schema_grants.sql applied. Proves, single-session (the genuinely-parallel
-- shared-window-not-N× race lives in scripts/p4b_rate_concurrency.sh):
--   (a) a single window admits EXACTLY max_count then refuses (binding dim_key + reset);
--   (b) max_count = 0 refuses the FIRST request (0 < 0 false; nothing consumed);
--   (c) keyed: team A over its limit does NOT affect team B (distinct dim_key);
--   (d) window rollover: a new window_id starts fresh (the key governs the counter);
--   (e) multi-dim all-or-nothing: team at cap + a headroom project => refused binding
--       team, NEITHER window consumed;
--   (f) reset_epoch equals the window's next boundary;
--   (g) policy read RLS (a team-lead reads only its own team's / projects' policy);
--   (h) admin-only policy set.
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p4b_rate_rls_test.sql
--
-- Uses SET ROLE aimee_kb_runtime for the RLS-read assertions (a superuser session
-- exercises the non-owner runtime role's RLS view). The correctness assertions run as the
-- owner principal (the admin gate for org_rate_policy_set). One transaction, rolled back.

\set ON_ERROR_STOP on

BEGIN;

SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (949201, 'p4b_alpha'), (949202, 'p4b_beta');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:p4blead_a', 949201, 1), ('oidc:test:p4blead_b', 949202, 1);
INSERT INTO kb_team_lead(identity_key, team)
  VALUES ('oidc:test:p4blead_a', 949201), ('oidc:test:p4blead_b', 949202);
INSERT INTO kb_project(id, parent, name) VALUES (949210, 949201, 'p4b_alpha_proj');

-- Schema posture: policy + window carry row security; runtime is NOBYPASSRLS; the policy
-- expression UNIQUE INDEX exists and the window composite PK exists.
DO $$
BEGIN
  IF (SELECT rolbypassrls FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE EXCEPTION 'P4b FAIL: aimee_kb_runtime has BYPASSRLS';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_rate_policy') IS NOT TRUE THEN
    RAISE EXCEPTION 'P4b FAIL: org_rate_policy has no row security';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_rate_window') IS NOT TRUE THEN
    RAISE EXCEPTION 'P4b FAIL: org_rate_window has no row security';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_indexes WHERE indexname = 'idx_org_rate_policy_key') THEN
    RAISE EXCEPTION 'P4b FAIL: org_rate_policy UNIQUE INDEX missing';
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (a) single window admits exactly max_count then refuses; (f) reset_epoch boundary.
-- ----------------------------------------------------------------------------
DO $$
DECLARE v_adm BOOLEAN; v_bind TEXT; v_reset BIGINT; v_cnt BIGINT; v_expect BIGINT;
BEGIN
  PERFORM org_rate_policy_set('team', '949201', 100, 2);
  SELECT admitted INTO v_adm FROM org_rate_check(949201, NULL, NULL, NULL);
  IF NOT v_adm THEN RAISE EXCEPTION 'P4b FAIL: check 1 not admitted'; END IF;
  SELECT admitted INTO v_adm FROM org_rate_check(949201, NULL, NULL, NULL);
  IF NOT v_adm THEN RAISE EXCEPTION 'P4b FAIL: check 2 not admitted'; END IF;
  -- The 3rd exceeds max=2 -> refused, binding the team dim_key, with the window reset.
  SELECT admitted, binding_dim, reset_epoch INTO v_adm, v_bind, v_reset
    FROM org_rate_check(949201, NULL, NULL, NULL);
  IF v_adm THEN RAISE EXCEPTION 'P4b FAIL: check 3 admitted (over max=2)'; END IF;
  IF v_bind <> 'team:949201' THEN RAISE EXCEPTION 'P4b FAIL: binding=% (want team:949201)', v_bind; END IF;
  -- (f) reset_epoch = (floor(epoch/window)+1)*window for the 100s window.
  v_expect := (floor(extract(epoch from now()) / 100)::bigint + 1) * 100;
  IF v_reset <> v_expect THEN RAISE EXCEPTION 'P4b FAIL: reset_epoch=% (want %)', v_reset, v_expect; END IF;
  -- The window counter is EXACTLY 2 (the refusal consumed nothing).
  SELECT max(count) INTO v_cnt FROM org_rate_window WHERE dim_key = 'team:949201';
  IF v_cnt <> 2 THEN RAISE EXCEPTION 'P4b FAIL: window count=% (want 2)', v_cnt; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (b) max_count = 0 refuses the FIRST request; nothing consumed.
-- ----------------------------------------------------------------------------
DO $$
DECLARE v_adm BOOLEAN; v_bind TEXT; v_cnt BIGINT;
BEGIN
  PERFORM org_rate_policy_set('team', '949202', 100, 0);  -- always-deny
  SELECT admitted, binding_dim INTO v_adm, v_bind FROM org_rate_check(949202, NULL, NULL, NULL);
  IF v_adm THEN RAISE EXCEPTION 'P4b FAIL: max=0 admitted the first request'; END IF;
  IF v_bind <> 'team:949202' THEN RAISE EXCEPTION 'P4b FAIL: max=0 binding=% (want team:949202)', v_bind; END IF;
  SELECT COALESCE(max(count), 0) INTO v_cnt FROM org_rate_window WHERE dim_key = 'team:949202';
  IF v_cnt <> 0 THEN RAISE EXCEPTION 'P4b FAIL: max=0 window count=% (want 0)', v_cnt; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (c) keyed independence: team A exhausted does NOT affect team B (distinct dim_key).
-- ----------------------------------------------------------------------------
DO $$
DECLARE v_adm BOOLEAN;
BEGIN
  -- Reset team A (949201) to max=1 for a clean keyed check, then exhaust it.
  DELETE FROM org_rate_window WHERE dim_key = 'team:949201';
  PERFORM org_rate_policy_set('team', '949201', 100, 1);
  -- Team B uses a fresh id with its own policy.
  INSERT INTO kb_team(id, name) VALUES (949203, 'p4b_gamma');
  PERFORM org_rate_policy_set('team', '949203', 100, 1);

  SELECT admitted INTO v_adm FROM org_rate_check(949201, NULL, NULL, NULL);  -- A: 1st ok
  IF NOT v_adm THEN RAISE EXCEPTION 'P4b FAIL: keyed A first not admitted'; END IF;
  SELECT admitted INTO v_adm FROM org_rate_check(949201, NULL, NULL, NULL);  -- A: 2nd refused
  IF v_adm THEN RAISE EXCEPTION 'P4b FAIL: keyed A second admitted (over max=1)'; END IF;
  -- B is a DIFFERENT dim_key: still admits its first request.
  SELECT admitted INTO v_adm FROM org_rate_check(949203, NULL, NULL, NULL);
  IF NOT v_adm THEN RAISE EXCEPTION 'P4b FAIL: keyed B first not admitted (A leaked into B)'; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (d) window rollover: a new window_id starts fresh (the key governs the counter).
-- ----------------------------------------------------------------------------
DO $$
DECLARE v_adm BOOLEAN; v_cnt BIGINT; v_wid TEXT;
BEGIN
  -- team A (949201) is at max=1 from (c). Simulate a window rollover by re-keying the
  -- current window row to the PREVIOUS bucket, so the CURRENT bucket has no counter.
  SELECT window_id INTO v_wid FROM org_rate_window WHERE dim_key = 'team:949201' ORDER BY window_id DESC LIMIT 1;
  UPDATE org_rate_window
     SET window_id = '100:' || (split_part(v_wid, ':', 2)::bigint - 1)::text
   WHERE dim_key = 'team:949201' AND window_id = v_wid;
  -- A check in the NEW (current) window admits again — it does not see the old count.
  SELECT admitted INTO v_adm FROM org_rate_check(949201, NULL, NULL, NULL);
  IF NOT v_adm THEN RAISE EXCEPTION 'P4b FAIL: rollover did not reset the window'; END IF;
  SELECT count INTO v_cnt FROM org_rate_window WHERE dim_key = 'team:949201' AND window_id = v_wid;
  IF v_cnt <> 1 THEN RAISE EXCEPTION 'P4b FAIL: new-window count=% (want 1)', v_cnt; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (e) multi-dim all-or-nothing: team at cap + a headroom project => refused binding team,
--     NEITHER window consumed.
-- ----------------------------------------------------------------------------
DO $$
DECLARE v_adm BOOLEAN; v_bind TEXT; team_cnt BIGINT; proj_cnt BIGINT;
BEGIN
  -- Fresh team with max=1 (exhausted) + a project with generous headroom.
  INSERT INTO kb_team(id, name) VALUES (949204, 'p4b_delta');
  INSERT INTO kb_project(id, parent, name) VALUES (949214, 949204, 'p4b_delta_proj');
  PERFORM org_rate_policy_set('team', '949204', 100, 1);
  PERFORM org_rate_policy_set('project', '949214', 100, 100);
  -- Exhaust the team dim alone.
  SELECT admitted INTO v_adm FROM org_rate_check(949204, NULL, NULL, NULL);
  IF NOT v_adm THEN RAISE EXCEPTION 'P4b FAIL: multi-dim setup team check not admitted'; END IF;
  -- Now a multi-dim check (team + project). Deterministic dim_key order locks/checks
  -- 'project:949214' (headroom) then 'team:949204' (at cap) -> team binds -> refused,
  -- and NEITHER window is incremented (all-or-nothing).
  SELECT admitted, binding_dim INTO v_adm, v_bind FROM org_rate_check(949204, 949214, NULL, NULL);
  IF v_adm THEN RAISE EXCEPTION 'P4b FAIL: multi-dim admitted despite team at cap'; END IF;
  IF v_bind <> 'team:949204' THEN RAISE EXCEPTION 'P4b FAIL: multi-dim binding=% (want team:949204)', v_bind; END IF;
  SELECT max(count) INTO team_cnt FROM org_rate_window WHERE dim_key = 'team:949204';
  IF team_cnt <> 1 THEN RAISE EXCEPTION 'P4b FAIL: team window consumed by refusal (count=% want 1)', team_cnt; END IF;
  SELECT COALESCE(max(count), 0) INTO proj_cnt FROM org_rate_window WHERE dim_key = 'project:949214';
  IF proj_cnt <> 0 THEN RAISE EXCEPTION 'P4b FAIL: headroom project window consumed by refusal (count=% want 0)', proj_cnt; END IF;
END $$;

-- A dim with NO policy is skipped (a model with no policy never refuses/creates a window).
DO $$
DECLARE v_adm BOOLEAN;
BEGIN
  INSERT INTO kb_team(id, name) VALUES (949205, 'p4b_eps');
  PERFORM org_rate_policy_set('team', '949205', 100, 5);
  SELECT admitted INTO v_adm FROM org_rate_check(949205, NULL, 'claude-opus-no-policy', NULL);
  IF NOT v_adm THEN RAISE EXCEPTION 'P4b FAIL: no-policy model dim was enforced'; END IF;
  IF EXISTS (SELECT 1 FROM org_rate_window WHERE dim_key = 'model:claude-opus-no-policy') THEN
    RAISE EXCEPTION 'P4b FAIL: a no-policy model dim created a window';
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (g) policy read RLS: a team-lead reads only its own team's / projects' policy.
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:p4blead_a', 949201);
DO $$
DECLARE own_n INT; other_n INT; win_n INT;
BEGIN
  -- Direct RLS view of org_rate_policy: lead_a sees its own team row, not team B's.
  SELECT count(*) INTO own_n   FROM org_rate_policy WHERE dim = 'team' AND scope_key = '949201';
  SELECT count(*) INTO other_n FROM org_rate_policy WHERE dim = 'team' AND scope_key = '949202';
  IF own_n <> 1 OR other_n <> 0 THEN
    RAISE EXCEPTION 'P4b FAIL: policy RLS lead_a own=% other=% (want 1/0)', own_n, other_n;
  END IF;
  -- org_rate_window is definer-only: runtime holds NO SELECT grant (REVOKE'd), so a
  -- direct read is denied at the privilege layer (stronger than an RLS-filtered 0 rows).
  BEGIN
    SELECT count(*) INTO win_n FROM org_rate_window;
    RAISE EXCEPTION 'P4b FAIL: runtime performed a direct SELECT on org_rate_window (want denied)';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: definer-only
  END;
  -- Runtime cannot forge a direct policy write (definer-only path).
  BEGIN
    INSERT INTO org_rate_policy(dim, scope_key, window_seconds, max_count)
      VALUES ('team', '949201', 1, 999);
    RAISE EXCEPTION 'P4b FAIL: runtime forged a direct org_rate_policy INSERT';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: no INSERT grant
  END;
END $$;

-- org_rate_policy_show is actor-bound: lead_a may read its own team policy, NOT team B's.
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM org_rate_policy_show('team', '949201');
  IF n <> 1 THEN RAISE EXCEPTION 'P4b FAIL: lead_a show(own team) returned % rows (want 1)', n; END IF;
  BEGIN
    PERFORM org_rate_policy_show('team', '949202');
    RAISE EXCEPTION 'P4b FAIL: lead_a read team 949202 policy (cross-team)';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected 42501
  END;
  -- A global dim (model) is admin-only: a lead is refused.
  BEGIN
    PERFORM org_rate_policy_show('model', 'claude-opus');
    RAISE EXCEPTION 'P4b FAIL: lead_a read a model policy (admin-only dim)';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected 42501
  END;
END $$;

-- ----------------------------------------------------------------------------
-- (h) admin-only policy set: a non-admin runtime principal is refused.
-- ----------------------------------------------------------------------------
DO $$
BEGIN
  BEGIN
    PERFORM org_rate_policy_set('team', '949201', 100, 9);
    RAISE EXCEPTION 'P4b FAIL: non-admin org_rate_policy_set was accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected 42501
  END;
END $$;
RESET ROLE;

\echo '== P4b keyed fixed-window rate limiter + RLS assertions PASSED =='
ROLLBACK;
