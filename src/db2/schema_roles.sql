-- schema_roles.sql: P1 tenancy — the three-role Postgres split (invariant #10 / I2).
--
-- Applied OUT OF BAND by the migration/owner path (`aimee-kb migrate`), NEVER by
-- the runtime service. It establishes the least-privilege split that makes the
-- team-scoped RLS in schema.sql load-bearing:
--
--   * owner role   (aimee_kb_owner)   — owns the tables; used only by migrations.
--   * migrate role (aimee_kb_migrate) — DDL only, run out of band. Member of owner.
--   * runtime role (aimee_kb_runtime) — DML only, NON-owner, NOBYPASSRLS, no CREATE.
--       The service connects as this. Because it is a non-owner with NOBYPASSRLS
--       and every tenant table is FORCE ROW LEVEL SECURITY, Postgres owner-bypass
--       cannot defeat the team predicate.
--
-- Passwords/credentials are provisioned by the deploy (each role a distinct
-- credential); this file only creates the roles idempotently and grants the
-- privilege split. Role names may be overridden at provisioning by textual
-- substitution of the __KB_{OWNER,MIGRATE,RUNTIME}__ tokens; unsubstituted they
-- default to the canonical names below.

-- Idempotent role creation (Postgres has no CREATE ROLE IF NOT EXISTS).
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_owner') THEN
    CREATE ROLE aimee_kb_owner NOLOGIN;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_migrate') THEN
    CREATE ROLE aimee_kb_migrate NOLOGIN;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_runtime') THEN
    CREATE ROLE aimee_kb_runtime NOLOGIN;
  END IF;
END
$$;

-- The runtime role must never bypass RLS and must never hold DDL. These are the
-- properties db2_init boot-asserts (B4/N4): a mismatch here or an operator
-- over-grant is caught at boot, not silently tolerated.
ALTER ROLE aimee_kb_runtime NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER;
ALTER ROLE aimee_kb_migrate NOBYPASSRLS NOSUPERUSER;

-- migrate acts as owner for DDL; runtime never does.
GRANT aimee_kb_owner TO aimee_kb_migrate;

-- Schema usage: runtime may resolve objects but NOT create them.
GRANT USAGE ON SCHEMA public TO aimee_kb_runtime;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_runtime;

-- DML on existing + future tables for the runtime role. DDL (owner) is excluded.
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO aimee_kb_runtime;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO aimee_kb_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_kb_owner IN SCHEMA public
  GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO aimee_kb_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_kb_owner IN SCHEMA public
  GRANT USAGE, SELECT ON SEQUENCES TO aimee_kb_runtime;

-- The append-only WORM audit store: runtime may INSERT/SELECT only (no UPDATE/
-- DELETE), matching the existing kb_audit_event writer-role contract.
REVOKE UPDATE, DELETE ON kb_audit_event FROM aimee_kb_runtime;
