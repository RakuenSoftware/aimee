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
END
$$;
