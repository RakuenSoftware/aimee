-- schema_grants.sql: P1 hardened-tier runtime GRANTs (phase 3 of provisioning).
--
-- Applied AFTER schema_roles.sql (roles exist) AND schema.sql (tables exist), by
-- the migration/owner path — NEVER by the runtime service. Gives the non-owner,
-- NOBYPASSRLS runtime role DML on the tenant tables (RLS still constrains every
-- row), INSERT/SELECT-only on the WORM audit store, sequence usage, and EXECUTE on
-- the one context setter. Dev/single-owner deployments skip this file entirely
-- (they run no three-role split); it is a no-op-safe re-run on a hardened tier.

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    RAISE NOTICE 'schema_grants: aimee_kb_runtime absent (dev tier) — skipping grants';
    RETURN;
  END IF;

  -- DML on existing + future tables; DDL (owner) excluded.
  GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO aimee_kb_runtime;
  -- The primary maintenance barrier is owner-only even though the legacy grant
  -- bootstrap is intentionally broad.  Keep this revoke adjacent so reapplication
  -- can never restore a runtime mutation or observation path.
  REVOKE ALL ON TABLE kb_vault_control FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_startup_status() FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_vault_control_startup_status() TO aimee_kb_runtime;
  -- P7-reseal-c is migration-owner orchestration only. The broad bootstrap grant
  -- above must never make its operation ledger, staged wraps, outbox, or helper
  -- functions visible to the runtime role on either first apply or re-apply.
  REVOKE ALL ON TABLE kb_vault_rewrap_operation, kb_vault_rewrap_dek_stage,
    kb_vault_rewrap_check_stage, kb_vault_rewrap_worm FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_rewrap_worm_block(),
    org_vault_rewrap_pack_text(TEXT), org_vault_rewrap_pack_bytes(BYTEA),
    org_vault_rewrap_worm_append(TEXT,TEXT,TEXT,TEXT),
    org_vault_rewrap_begin(TEXT,TEXT,TEXT,BIGINT,BIGINT),
    org_vault_rewrap_record_prepared(TEXT,BIGINT,BYTEA,BYTEA),
    org_vault_rewrap_assert_live(TEXT,BIGINT,BOOLEAN),
    org_vault_rewrap_status(TEXT),
    org_vault_rewrap_snapshot(TEXT),
    org_vault_rewrap_secret_page(TEXT,BIGINT,BIGINT,INTEGER),
    org_vault_rewrap_check_page(TEXT,BIGINT,BYTEA,INTEGER),
    org_vault_rewrap_stage_dek(TEXT,BIGINT,BIGINT,TEXT,TEXT,TEXT,BIGINT,BYTEA,BYTEA),
    org_vault_rewrap_stage_check(TEXT,BIGINT,TEXT,BYTEA,BYTEA),
    org_vault_rewrap_digests(TEXT), org_vault_rewrap_stage_finish(TEXT,BIGINT),
    org_vault_rewrap_mark_committing(TEXT,BIGINT),
    org_vault_rewrap_mark_resealed(TEXT,BIGINT,BYTEA),
    org_vault_rewrap_promote(TEXT,BIGINT),
    org_vault_rewrap_verify_summary(TEXT,BIGINT),
    org_vault_rewrap_verify_secret_page(TEXT,BIGINT,BIGINT,INTEGER),
    org_vault_rewrap_verify_check_page(TEXT,BIGINT,BYTEA,INTEGER),
    org_vault_rewrap_complete(TEXT,BIGINT,BYTEA,BYTEA,BYTEA),
    org_vault_rewrap_abort(TEXT,BIGINT,TEXT),
    org_vault_rewrap_recovery_required(TEXT,BIGINT,TEXT)
    FROM aimee_kb_runtime;
  GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO aimee_kb_runtime;
  ALTER DEFAULT PRIVILEGES FOR ROLE aimee_kb_owner IN SCHEMA public
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO aimee_kb_runtime;
  ALTER DEFAULT PRIVILEGES FOR ROLE aimee_kb_owner IN SCHEMA public
    GRANT USAGE, SELECT ON SEQUENCES TO aimee_kb_runtime;

  -- Explicit tenant-table grants (in case ALL TABLES ran before these existed).
  GRANT SELECT, INSERT, UPDATE, DELETE ON
    kb_team, kb_project, kb_team_membership, kb_project_membership,
    kb_admin_grant, kb_oidc_jwks TO aimee_kb_runtime;

  -- WORM audit store: runtime may INSERT/SELECT only, never UPDATE/DELETE.
  REVOKE UPDATE, DELETE, TRUNCATE ON kb_audit_event FROM aimee_kb_runtime;
  GRANT INSERT, SELECT ON kb_audit_event TO aimee_kb_runtime;

  -- set_tenant_context is the ONLY runtime-usable tenant-GUC setter; EXECUTE to
  -- the runtime role only, never PUBLIC (N4).
  REVOKE ALL ON FUNCTION set_tenant_context(TEXT, BIGINT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION set_tenant_context(TEXT, BIGINT) TO aimee_kb_runtime;

  -- P3a cost attribution. The ledger, rollup, and price tables are WRITTEN ONLY by
  -- the SECURITY DEFINER metering functions (owned by aimee_kb_owner, which bypasses
  -- ENABLE-not-FORCE RLS). Runtime therefore gets SELECT (RLS-filtered: admin OR
  -- team-lead) but its direct write grant from the ALL TABLES line above is REVOKED,
  -- so a compromised runtime session cannot forge or mutate cost rows out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON
    org_model_pricing, org_model_pricing_current, org_token_audit, org_spend_rollup
    FROM aimee_kb_runtime;
  GRANT SELECT ON
    org_model_pricing, org_model_pricing_current, org_token_audit, org_spend_rollup
    TO aimee_kb_runtime;
  -- kb_team_lead is an admin-written grant (RLS gates writes to admins), same posture
  -- as kb_admin_grant: runtime holds DML, RLS constrains it.
  GRANT SELECT, INSERT, UPDATE, DELETE ON kb_team_lead TO aimee_kb_runtime;

  -- The metering functions are the ONLY write path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_pricing_add_version(TEXT,TEXT,NUMERIC,NUMERIC,NUMERIC,NUMERIC) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_pricing_current_version(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_token_estimate_cost(TEXT,BIGINT,BIGINT,BIGINT,BIGINT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_token_audit_start(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,BIGINT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_token_audit_settle(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BIGINT,BIGINT,NUMERIC,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_pricing_add_version(TEXT,TEXT,NUMERIC,NUMERIC,NUMERIC,NUMERIC) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_pricing_current_version(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_token_estimate_cost(TEXT,BIGINT,BIGINT,BIGINT,BIGINT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_token_audit_start(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,BIGINT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_token_audit_settle(TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BIGINT,BIGINT,NUMERIC,TEXT) TO aimee_kb_runtime;

  -- P3b org spend reporting. The read-only aggregation definer enforces the admin/lead
  -- predicate INTERNALLY (it IS the authz gate — SECURITY DEFINER bypasses RLS), so it
  -- is the single authorized reporting path over org_spend_rollup; EXECUTE to runtime,
  -- never PUBLIC.
  REVOKE ALL ON FUNCTION org_spend_query(BIGINT,BIGINT,TEXT,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_spend_query(BIGINT,BIGINT,TEXT,TEXT) TO aimee_kb_runtime;

  -- P10 kb credential vault. The ciphertext store is WRITTEN ONLY by the SECURITY
  -- DEFINER vault functions (owned by aimee_kb_owner, which bypasses RLS for its own
  -- internal version scan). Runtime therefore gets SELECT (RLS-filtered: own-team rows
  -- / admin) but its direct write grant from the ALL TABLES line above is REVOKED, so a
  -- compromised runtime session cannot forge or mutate vault rows out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON
    org_vault_salt, org_vault_secret, org_vault_current FROM aimee_kb_runtime;
  GRANT SELECT ON
    org_vault_salt, org_vault_secret, org_vault_current TO aimee_kb_runtime;
  REVOKE SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON org_vault_rotation FROM aimee_kb_runtime;
  -- The vault definer functions are the ONLY write/read-through path; EXECUTE to
  -- runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_vault_salt_ensure(TEXT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_salt_read(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_kek_check_read(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_kek_check_set(TEXT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_put(TEXT,BIGINT,TEXT,TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_get_current(TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_has(TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_list(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_list_principals() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_delete(TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_current_wraps(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rewrap(TEXT,TEXT,TEXT,BIGINT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_authorized(TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,BIGINT,BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_stage(TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_transition(TEXT,BIGINT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_finalize(TEXT,BIGINT,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_get(BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_claim(TEXT,BIGINT,TEXT,TEXT,INTEGER) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_heartbeat(TEXT,BIGINT,TEXT,BIGINT,INTEGER) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_release(TEXT,BIGINT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_checkpoint_old_ref(TEXT,BIGINT,TEXT,BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_stage_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,BYTEA,BYTEA,BYTEA,BYTEA) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_probe_admit(TEXT,BIGINT,TEXT,BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_transition_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_fail_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_rotation_remediate(TEXT,BIGINT,TEXT,BIGINT,BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_key_use_candidate(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_key_use_admit(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BYTEA) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_vault_salt_ensure(TEXT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_salt_read(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_kek_check_read(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_kek_check_set(TEXT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_put(TEXT,BIGINT,TEXT,TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_get_current(TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_has(TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_list(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_list_principals() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_delete(TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_current_wraps(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rewrap(TEXT,TEXT,TEXT,BIGINT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,BIGINT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_stage(TEXT,BIGINT,BYTEA,BYTEA,BYTEA,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_transition(TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_finalize(TEXT,BIGINT,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_get(BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_claim(TEXT,BIGINT,TEXT,TEXT,INTEGER) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_heartbeat(TEXT,BIGINT,TEXT,BIGINT,INTEGER) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_release(TEXT,BIGINT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_checkpoint_old_ref(TEXT,BIGINT,TEXT,BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_stage_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,BYTEA,BYTEA,BYTEA,BYTEA) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_probe_admit(TEXT,BIGINT,TEXT,BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_transition_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_fail_claimed(TEXT,BIGINT,TEXT,BIGINT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_rotation_remediate(TEXT,BIGINT,TEXT,BIGINT,BIGINT,TEXT) TO aimee_kb_runtime;
  REVOKE SELECT,INSERT,UPDATE,DELETE,TRUNCATE ON org_vault_key_use_intent FROM aimee_kb_runtime;
  REVOKE ALL ON TABLE kb_vault_control FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_require_open() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_vault_control_lock_exclusive() FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_vault_control_startup_status() FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_vault_control_startup_status() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_key_use_candidate(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_vault_key_use_admit(TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BYTEA) TO aimee_kb_runtime;

  -- P2a org model catalog + entitlement. The catalog is admin-managed and read/written
  -- EXCLUSIVELY through the SECURITY DEFINER functions (owned by aimee_kb_owner, which
  -- bypasses ENABLE-not-FORCE RLS). Runtime gets NO direct catalog access AT ALL — not
  -- even SELECT — so every catalog read funnels through org_catalog_entitled(); its
  -- direct write grant from the ALL TABLES line above is REVOKED too. Entitlement direct
  -- SELECT stays (RLS-filtered: own-team OR admin) for defense-in-depth, but its writes
  -- are REVOKED (only the definer functions may mutate it).
  REVOKE SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON org_model_catalog FROM aimee_kb_runtime;
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON org_model_entitlement FROM aimee_kb_runtime;
  GRANT SELECT ON org_model_entitlement TO aimee_kb_runtime;
  -- The catalog CRUD + entitled-read functions are the ONLY access path; EXECUTE to
  -- runtime, never PUBLIC. kb_audit_worm_append is intentionally NOT granted to runtime
  -- (only the owner-run definer mutations call it) so runtime cannot forge audit rows.
  REVOKE ALL ON FUNCTION kb_audit_worm_append(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_entitled() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,BOOLEAN) FROM PUBLIC;
  -- P6c-catalog: the bedrock companion write path is the ONLY way to land a provider=
  -- 'bedrock' row (the plain org_catalog_upsert fail-closes on it). Runtime keeps NO direct
  -- catalog access; the new bedrock_* columns are therefore reachable ONLY via the definer
  -- (org_catalog_entitled()'s projection is UNCHANGED, so account/ARNs/region never leak to
  -- a tenant read). org_bedrock_adapter_supported is a pure predicate (no table access) —
  -- left PUBLIC-callable (harmless), plus an explicit runtime grant for clarity.
  REVOKE ALL ON FUNCTION org_catalog_bedrock_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT[],TEXT[],TEXT,BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_bedrock_target(BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_catalog_remove(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_model_entitle(TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_model_unentitle(TEXT,BIGINT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_catalog_entitled() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_bedrock_upsert(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT[],TEXT[],TEXT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_bedrock_target(BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_bedrock_adapter_supported(TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_catalog_remove(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_model_entitle(TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_model_unentitle(TEXT,BIGINT) TO aimee_kb_runtime;

  -- P4a budget reservation core. The four budget tables are WRITTEN ONLY by the SECURITY
  -- DEFINER reserve/settle/set functions (owned by aimee_kb_owner, which bypasses
  -- ENABLE-not-FORCE RLS). Runtime therefore gets SELECT (RLS-filtered: admin OR
  -- team-lead on config/counter, admin-only on reservation/alloc) but its direct write
  -- grant from the ALL TABLES line above is REVOKED, so a compromised runtime session
  -- cannot forge or mutate a reservation/counter out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON
    org_budget, org_budget_counter, org_budget_reservation, org_budget_reservation_alloc
    FROM aimee_kb_runtime;
  GRANT SELECT ON
    org_budget, org_budget_counter, org_budget_reservation, org_budget_reservation_alloc
    TO aimee_kb_runtime;
  -- The budget functions are the ONLY write path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_budget_period_id(TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_reserve(TEXT,TEXT,BIGINT,BIGINT,BIGINT,NUMERIC,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_settle(TEXT,TEXT,NUMERIC) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_settle_expired() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_heartbeat(TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_set(BIGINT,BIGINT,TEXT,NUMERIC,NUMERIC) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_budget_show(BIGINT,BIGINT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_budget_period_id(TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_reserve(TEXT,TEXT,BIGINT,BIGINT,BIGINT,NUMERIC,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_settle(TEXT,TEXT,NUMERIC) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_settle_expired() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_heartbeat(TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_set(BIGINT,BIGINT,TEXT,NUMERIC,NUMERIC) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_budget_show(BIGINT,BIGINT) TO aimee_kb_runtime;

  -- P4b keyed fixed-window rate limiter. org_rate_policy + org_rate_window are WRITTEN
  -- ONLY by the SECURITY DEFINER check/set functions (owned by aimee_kb_owner, which
  -- bypasses ENABLE-not-FORCE RLS). Runtime gets SELECT on the policy config
  -- (RLS-filtered: admin OR team-lead), but its direct write grant from the ALL TABLES
  -- line is REVOKED. org_rate_window is a definer-only operational counter: runtime gets
  -- NO direct read or write, so a compromised runtime session cannot read or forge the
  -- shared window counters out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON org_rate_policy FROM aimee_kb_runtime;
  GRANT SELECT ON org_rate_policy TO aimee_kb_runtime;
  REVOKE ALL ON org_rate_window FROM aimee_kb_runtime;
  -- The rate functions are the ONLY write path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_rate_check(BIGINT,BIGINT,TEXT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_rate_policy_set(TEXT,TEXT,INT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_rate_policy_show(TEXT,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_rate_check(BIGINT,BIGINT,TEXT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_rate_policy_set(TEXT,TEXT,INT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_rate_policy_show(TEXT,TEXT) TO aimee_kb_runtime;

  -- P2b-a egress authority. Runtime has no direct access to the private binding or
  -- durable dispatch ledger; every state change is owner-scoped and fenced by a
  -- SECURITY DEFINER function. The admin-gated binding setter is intentionally
  -- executable by runtime because it enforces kb_principal_is_admin internally.
  REVOKE ALL ON org_egress_binding, org_egress_dispatch FROM aimee_kb_runtime;
  REVOKE ALL ON FUNCTION org_egress_binding_set(BIGINT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_admit(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_dispatch_begin(TEXT,TEXT,TEXT,TEXT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_dispatch_heartbeat(BIGINT,TEXT,BIGINT,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_dispatch_settle(BIGINT,TEXT,BIGINT,TEXT,INT,BIGINT,BIGINT,BIGINT,BIGINT,TEXT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_egress_recover(INT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_egress_binding_set(BIGINT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_admit(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,BIGINT,BIGINT,TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_begin(TEXT,TEXT,TEXT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_heartbeat(BIGINT,TEXT,BIGINT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_owner_guard(BIGINT,TEXT,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_dispatch_settle(BIGINT,TEXT,BIGINT,TEXT,INT,BIGINT,BIGINT,BIGINT,BIGINT,TEXT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_egress_recover(INT) TO aimee_kb_runtime;

  -- Certificate renewal is a single SECURITY DEFINER lineage/grant/audit
  -- mutation. Runtime cannot invoke the underlying WORM appender directly.
  REVOKE ALL ON FUNCTION kb_enrollment_renew(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION kb_enrollment_renew(TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT) TO aimee_kb_runtime;

  -- P9a telemetry export + content-free ingest. org_telemetry + org_telemetry_allowlist
  -- are WRITTEN ONLY by the SECURITY DEFINER ingest/allow functions (owned by
  -- aimee_kb_owner, which bypasses ENABLE-not-FORCE RLS). Runtime gets SELECT on
  -- org_telemetry (RLS-filtered: admin OR team-lead of the row's team) for defense in
  -- depth, but its direct write grant from the ALL TABLES line is REVOKED. The allowlist
  -- is admin-managed: runtime gets NO direct access AT ALL (read via org_telemetry_allow_show,
  -- which enforces the admin gate). /v1/metrics never reads org_telemetry — it reads the
  -- authoritative rollup tables via org_metrics_snapshot (the write-only-target invariant).
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON org_telemetry FROM aimee_kb_runtime;
  GRANT SELECT ON org_telemetry TO aimee_kb_runtime;
  REVOKE SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON org_telemetry_allowlist FROM aimee_kb_runtime;
  -- The telemetry functions are the ONLY access path; EXECUTE to runtime, never PUBLIC.
  REVOKE ALL ON FUNCTION org_telemetry_ingest(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,NUMERIC,BIGINT) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_telemetry_allow(TEXT,TEXT[],BOOLEAN) FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_telemetry_allow_show() FROM PUBLIC;
  REVOKE ALL ON FUNCTION org_metrics_snapshot() FROM PUBLIC;
  GRANT EXECUTE ON FUNCTION org_telemetry_ingest(TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,NUMERIC,BIGINT) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_telemetry_allow(TEXT,TEXT[],BOOLEAN) TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_telemetry_allow_show() TO aimee_kb_runtime;
  GRANT EXECUTE ON FUNCTION org_metrics_snapshot() TO aimee_kb_runtime;
END
$$;
