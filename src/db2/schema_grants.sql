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

  -- P10 kb credential vault. The ciphertext store is WRITTEN ONLY by the SECURITY
  -- DEFINER vault functions (owned by aimee_kb_owner, which bypasses RLS for its own
  -- internal version scan). Runtime therefore gets SELECT (RLS-filtered: own-team rows
  -- / admin) but its direct write grant from the ALL TABLES line above is REVOKED, so a
  -- compromised runtime session cannot forge or mutate vault rows out of band.
  REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON
    org_vault_salt, org_vault_secret, org_vault_current FROM aimee_kb_runtime;
  GRANT SELECT ON
    org_vault_salt, org_vault_secret, org_vault_current TO aimee_kb_runtime;
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
END
$$;
