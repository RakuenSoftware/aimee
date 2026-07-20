-- p9_telemetry_rls_test.sql: the DB-layer P9a telemetry correctness + isolation +
-- PII-structural gate. Run against a Postgres that has had schema_roles.sql +
-- schema.sql + schema_grants.sql applied. Proves, single-session:
--   (a) an allowlisted ingest -> 'stored' (1 row);
--   (b) an unknown event_schema -> 'dropped' (0 rows);
--   (c) an unknown metric_name for a KNOWN event_schema -> 'dropped';
--   (d) a duplicate source_event_id re-ingest -> 'deduped' (still 1 row, ON CONFLICT);
--   (e) a disallowed-charset / over-length metric_name -> 'dropped';
--   (f) team-scoped read RLS (a team-lead sees only its own team's telemetry; a
--       NULL-team org row is admin-only);
--   (g) admin-only allowlist mutation + read (a non-admin runtime principal is blocked;
--       a direct allowlist SELECT is privilege-denied);
--   (h) org_telemetry is content-free by construction — NO payload/content/sub/jsonb/
--       extra column (information_schema.columns column-set assertion);
--   (i) org_metrics_snapshot() reconciles with seeded org_spend_rollup / org_budget /
--       org_budget_counter rows;
--   (j) org_telemetry rows are NEVER read back through the metrics snapshot (the
--       write-only-target invariant: ingested metric names do not appear in /metrics).
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p9_telemetry_rls_test.sql
--
-- Uses SET ROLE aimee_kb_runtime for the RLS-read + non-admin assertions (a superuser
-- session exercises the non-owner runtime role's RLS view). The correctness assertions
-- run as the owner principal (admin, for org_telemetry_allow / org_metrics_snapshot).
-- One transaction, rolled back.

\set ON_ERROR_STOP on

BEGIN;

SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (949301, 'p9_alpha'), (949302, 'p9_beta');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:p9lead_a', 949301, 1), ('oidc:test:p9lead_b', 949302, 1);
INSERT INTO kb_team_lead(identity_key, team)
  VALUES ('oidc:test:p9lead_a', 949301), ('oidc:test:p9lead_b', 949302);

-- Schema posture: telemetry + allowlist carry row security; runtime is NOBYPASSRLS;
-- the idempotency UNIQUE INDEX exists.
DO $$
BEGIN
  IF (SELECT rolbypassrls FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE EXCEPTION 'P9 FAIL: aimee_kb_runtime has BYPASSRLS';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_telemetry') IS NOT TRUE THEN
    RAISE EXCEPTION 'P9 FAIL: org_telemetry has no row security';
  END IF;
  IF (SELECT relrowsecurity FROM pg_class WHERE relname = 'org_telemetry_allowlist') IS NOT TRUE THEN
    RAISE EXCEPTION 'P9 FAIL: org_telemetry_allowlist has no row security';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_indexes WHERE indexname = 'idx_org_telemetry_source') THEN
    RAISE EXCEPTION 'P9 FAIL: org_telemetry source UNIQUE INDEX missing';
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (h) org_telemetry is content-free BY CONSTRUCTION: no payload/content/sub/jsonb/
-- extra column exists; the expected content-free columns do.
-- ----------------------------------------------------------------------------
DO $$
DECLARE cols TEXT[];
BEGIN
  SELECT array_agg(column_name ORDER BY column_name) INTO cols
    FROM information_schema.columns WHERE table_name = 'org_telemetry';
  IF cols && ARRAY['payload','content','sub','jsonb','extra','detail','body','data'] THEN
    RAISE EXCEPTION 'P9 FAIL: org_telemetry has a content/PII column: %', cols;
  END IF;
  IF NOT (cols @> ARRAY['source_event_id','origin_cert_cn','metric_name','metric_kind',
                        'value','ts','team_id','event_schema']) THEN
    RAISE EXCEPTION 'P9 FAIL: org_telemetry missing an expected column (got %)', cols;
  END IF;
END $$;

-- Seed the forward allowlist (admin/definer). Only 'agent.metrics.v1' with
-- {tokens_total, cost_usd} is accepted; everything else drops.
SELECT org_telemetry_allow('agent.metrics.v1', ARRAY['tokens_total','cost_usd'], true);

-- ----------------------------------------------------------------------------
-- (a) allowlisted ingest -> stored; (b) unknown schema -> dropped; (c) unknown
-- metric for a known schema -> dropped; (e) bad-charset / over-length -> dropped.
-- ----------------------------------------------------------------------------
DO $$
DECLARE r TEXT; c BIGINT;
BEGIN
  -- (a) allowlisted metric for team 949301.
  r := org_telemetry_ingest('evt-1', 'cn-forwarder', 949301, 'agent.metrics.v1',
                            'tokens_total', 'counter', 100, 1700000000);
  IF r <> 'stored' THEN RAISE EXCEPTION 'P9 FAIL: (a) allowlisted ingest = % (want stored)', r; END IF;
  SELECT count(*) INTO c FROM org_telemetry WHERE source_event_id = 'evt-1';
  IF c <> 1 THEN RAISE EXCEPTION 'P9 FAIL: (a) stored row count = % (want 1)', c; END IF;

  -- (b) unknown event_schema -> dropped, nothing stored.
  r := org_telemetry_ingest('evt-2', 'cn', 949301, 'unknown.schema',
                            'tokens_total', 'counter', 5, 1700000001);
  IF r <> 'dropped' THEN RAISE EXCEPTION 'P9 FAIL: (b) unknown schema = % (want dropped)', r; END IF;
  SELECT count(*) INTO c FROM org_telemetry WHERE source_event_id = 'evt-2';
  IF c <> 0 THEN RAISE EXCEPTION 'P9 FAIL: (b) unknown-schema row stored (count = %)', c; END IF;

  -- (c) unknown metric_name for a KNOWN schema -> dropped.
  r := org_telemetry_ingest('evt-3', 'cn', 949301, 'agent.metrics.v1',
                            'not_allowlisted', 'counter', 5, 1700000002);
  IF r <> 'dropped' THEN RAISE EXCEPTION 'P9 FAIL: (c) unknown metric = % (want dropped)', r; END IF;
  SELECT count(*) INTO c FROM org_telemetry WHERE source_event_id = 'evt-3';
  IF c <> 0 THEN RAISE EXCEPTION 'P9 FAIL: (c) unknown-metric row stored (count = %)', c; END IF;

  -- (e) disallowed charset -> dropped.
  r := org_telemetry_ingest('evt-5', 'cn', 949301, 'agent.metrics.v1',
                            'bad name!', 'counter', 5, 1700000003);
  IF r <> 'dropped' THEN RAISE EXCEPTION 'P9 FAIL: (e) bad-charset = % (want dropped)', r; END IF;
  -- (e) over-length (129 chars) -> dropped.
  r := org_telemetry_ingest('evt-6', 'cn', 949301, 'agent.metrics.v1',
                            repeat('a', 129), 'counter', 5, 1700000004);
  IF r <> 'dropped' THEN RAISE EXCEPTION 'P9 FAIL: (e) over-length = % (want dropped)', r; END IF;
  -- bad metric_kind -> dropped.
  r := org_telemetry_ingest('evt-7', 'cn', 949301, 'agent.metrics.v1',
                            'tokens_total', 'histogram', 5, 1700000005);
  IF r <> 'dropped' THEN RAISE EXCEPTION 'P9 FAIL: (e) bad metric_kind = % (want dropped)', r; END IF;
  SELECT count(*) INTO c FROM org_telemetry WHERE source_event_id IN ('evt-5','evt-6','evt-7');
  IF c <> 0 THEN RAISE EXCEPTION 'P9 FAIL: (e) a malformed ingest stored a row (count = %)', c; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (d) duplicate source_event_id re-ingest -> deduped (still 1 row).
-- ----------------------------------------------------------------------------
DO $$
DECLARE r TEXT; c BIGINT;
BEGIN
  r := org_telemetry_ingest('evt-1', 'cn-forwarder', 949301, 'agent.metrics.v1',
                            'tokens_total', 'counter', 100, 1700000000);
  IF r <> 'deduped' THEN RAISE EXCEPTION 'P9 FAIL: (d) re-ingest = % (want deduped)', r; END IF;
  SELECT count(*) INTO c FROM org_telemetry WHERE source_event_id = 'evt-1';
  IF c <> 1 THEN RAISE EXCEPTION 'P9 FAIL: (d) dedupe left % rows (want 1)', c; END IF;
END $$;

-- Seed a second team's row + a NULL-team (org-level) row for the RLS read test.
DO $$
DECLARE r TEXT;
BEGIN
  r := org_telemetry_ingest('evt-b1', 'cn', 949302, 'agent.metrics.v1',
                            'cost_usd', 'gauge', 2.5, 1700000010);
  IF r <> 'stored' THEN RAISE EXCEPTION 'P9 FAIL: seed team-B ingest = % (want stored)', r; END IF;
  r := org_telemetry_ingest('evt-null', 'cn', NULL, 'agent.metrics.v1',
                            'cost_usd', 'gauge', 9.9, 1700000011);
  IF r <> 'stored' THEN RAISE EXCEPTION 'P9 FAIL: seed NULL-team ingest = % (want stored)', r; END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (i) org_metrics_snapshot reconciles with seeded authoritative-state rows, and
-- (j) org_telemetry rows are NEVER in the snapshot (write-only-target invariant).
-- ----------------------------------------------------------------------------
INSERT INTO org_spend_rollup(team_id, billable_model, day, cost_usd, row_count)
  VALUES (949301, 'model-x', '2026-01-01', 12.5, 3);
INSERT INTO org_budget(team_id, period, limit_usd) VALUES (949301, 'month', 1000);
INSERT INTO org_budget_counter(team_id, period, period_id, spend_usd, reserved_usd)
  VALUES (949301, 'month', '2026-01', 30, 5);

DO $$
DECLARE v NUMERIC; n BIGINT;
BEGIN
  SELECT value INTO v FROM org_metrics_snapshot()
    WHERE metric = 'aimee_org_spend_usd' AND team_id = 949301;
  IF v IS DISTINCT FROM 12.5 THEN RAISE EXCEPTION 'P9 FAIL: (i) spend snapshot = % (want 12.5)', v; END IF;
  SELECT value INTO v FROM org_metrics_snapshot()
    WHERE metric = 'aimee_org_budget_limit_usd' AND team_id = 949301 AND period = 'month';
  IF v IS DISTINCT FROM 1000 THEN RAISE EXCEPTION 'P9 FAIL: (i) budget limit snapshot = % (want 1000)', v; END IF;
  SELECT value INTO v FROM org_metrics_snapshot()
    WHERE metric = 'aimee_org_budget_spend_usd' AND team_id = 949301 AND period = 'month';
  IF v IS DISTINCT FROM 30 THEN RAISE EXCEPTION 'P9 FAIL: (i) budget spend snapshot = % (want 30)', v; END IF;
  SELECT value INTO v FROM org_metrics_snapshot()
    WHERE metric = 'aimee_org_budget_reserved_usd' AND team_id = 949301 AND period = 'month';
  IF v IS DISTINCT FROM 5 THEN RAISE EXCEPTION 'P9 FAIL: (i) budget reserved snapshot = % (want 5)', v; END IF;
  SELECT value INTO v FROM org_metrics_snapshot() WHERE metric = 'aimee_org_teams';
  IF v < 2 THEN RAISE EXCEPTION 'P9 FAIL: (i) teams snapshot = % (want >= 2)', v; END IF;

  -- (j) the snapshot emits ONLY the authoritative aimee_org_* series; the ingested
  -- telemetry metric names (tokens_total / cost_usd) never appear.
  SELECT count(*) INTO n FROM org_metrics_snapshot()
    WHERE metric NOT LIKE 'aimee_org_%';
  IF n <> 0 THEN RAISE EXCEPTION 'P9 FAIL: (j) snapshot emitted a non-authoritative metric'; END IF;
  IF EXISTS (SELECT 1 FROM org_metrics_snapshot() WHERE metric IN ('tokens_total','cost_usd')) THEN
    RAISE EXCEPTION 'P9 FAIL: (j) an ingested telemetry metric leaked into /metrics';
  END IF;
END $$;

-- ----------------------------------------------------------------------------
-- (f) team-scoped read RLS + (g) admin-only allowlist: as the non-owner runtime role.
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:p9lead_a', 949301);
DO $$
DECLARE own_n BIGINT; other_n BIGINT; null_n BIGINT;
BEGIN
  -- (f) lead_a sees its own team's rows, NOT team B's, NOT the NULL-team org row.
  SELECT count(*) INTO own_n   FROM org_telemetry WHERE team_id = 949301;
  SELECT count(*) INTO other_n FROM org_telemetry WHERE team_id = 949302;
  SELECT count(*) INTO null_n  FROM org_telemetry WHERE source_event_id = 'evt-null';
  IF own_n < 1 THEN RAISE EXCEPTION 'P9 FAIL: (f) lead_a sees no own-team rows (%)', own_n; END IF;
  IF other_n <> 0 THEN RAISE EXCEPTION 'P9 FAIL: (f) lead_a saw team-B rows (% ; cross-team leak)', other_n; END IF;
  IF null_n <> 0 THEN RAISE EXCEPTION 'P9 FAIL: (f) lead_a saw the NULL-team org row (admin-only)'; END IF;

  -- (g) a direct allowlist SELECT is privilege-denied (runtime holds NO grant).
  BEGIN
    PERFORM 1 FROM org_telemetry_allowlist;
    RAISE EXCEPTION 'P9 FAIL: (g) runtime performed a direct org_telemetry_allowlist SELECT';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected
  END;

  -- (g) a non-admin allowlist mutation is refused (42501 inside the definer).
  BEGIN
    PERFORM org_telemetry_allow('agent.metrics.v1', ARRAY['tokens_total'], true);
    RAISE EXCEPTION 'P9 FAIL: (g) non-admin org_telemetry_allow was accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected 42501
  END;

  -- (g) a non-admin allowlist read (definer) is refused.
  BEGIN
    PERFORM org_telemetry_allow_show();
    RAISE EXCEPTION 'P9 FAIL: (g) non-admin org_telemetry_allow_show was accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected 42501
  END;

  -- a non-admin metrics snapshot is refused (admin-only aggregate).
  BEGIN
    PERFORM org_metrics_snapshot();
    RAISE EXCEPTION 'P9 FAIL: (g) non-admin org_metrics_snapshot was accepted';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected 42501
  END;
END $$;
RESET ROLE;

\echo '== P9a telemetry correctness + RLS + PII-structural assertions PASSED =='
ROLLBACK;
