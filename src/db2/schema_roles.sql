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
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_status') THEN
    CREATE ROLE aimee_kb_status NOLOGIN NOINHERIT;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_status_login') THEN
    CREATE ROLE aimee_kb_status_login NOLOGIN NOINHERIT;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_status_definer') THEN
    CREATE ROLE aimee_kb_status_definer NOLOGIN NOINHERIT BYPASSRLS;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_token_roots_provision') THEN
    CREATE ROLE aimee_kb_token_roots_provision NOLOGIN NOINHERIT;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_jwks_publish') THEN
    CREATE ROLE aimee_kb_jwks_publish NOLOGIN NOINHERIT;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_jwks_runtime_definer') THEN
    CREATE ROLE aimee_kb_jwks_runtime_definer NOLOGIN NOINHERIT BYPASSRLS;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_token_authority_definer') THEN
    CREATE ROLE aimee_kb_token_authority_definer NOLOGIN NOINHERIT BYPASSRLS;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_token_authority_runtime') THEN
    CREATE ROLE aimee_kb_token_authority_runtime LOGIN NOINHERIT NOBYPASSRLS;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_token_authority_store_owner') THEN
    CREATE ROLE aimee_kb_token_authority_store_owner NOLOGIN NOINHERIT NOBYPASSRLS;
  END IF;
END
$$;

-- The runtime role must never bypass RLS and must never hold DDL. These are the
-- properties db2_init boot-asserts (B4/N4): a mismatch here or an operator
-- over-grant is caught at boot, not silently tolerated.
ALTER ROLE aimee_kb_runtime NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER;
ALTER ROLE aimee_kb_status NOLOGIN NOINHERIT NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
ALTER ROLE aimee_kb_status_login NOLOGIN NOINHERIT NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
ALTER ROLE aimee_kb_migrate NOBYPASSRLS NOSUPERUSER;
ALTER ROLE aimee_kb_owner NOLOGIN NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
-- This non-login role owns only the four fixed status-authority functions. Its
-- BYPASSRLS is required for their exact enrollment/registry checks across FORCE
-- RLS and is not inherited by the execution role.
ALTER ROLE aimee_kb_status_definer NOLOGIN NOINHERIT BYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
ALTER ROLE aimee_kb_token_roots_provision NOLOGIN NOINHERIT NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
ALTER ROLE aimee_kb_jwks_publish NOLOGIN NOINHERIT NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
-- This role owns only the certificate-authenticated public JWKS reader.  Its
-- BYPASSRLS authority is not inherited by runtime and is constrained by exact
-- object grants in schema_grants.sql.
ALTER ROLE aimee_kb_jwks_runtime_definer NOLOGIN NOINHERIT BYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
-- The dedicated online token-authority process connects as the runtime role.
-- Only the non-login function owner crosses FORCE RLS; neither role is inherited
-- by the ordinary kb service role.
ALTER ROLE aimee_kb_token_authority_definer NOLOGIN NOINHERIT BYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
ALTER ROLE aimee_kb_token_authority_runtime LOGIN NOINHERIT NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
ALTER ROLE aimee_kb_token_authority_store_owner NOLOGIN NOINHERIT NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;

-- migrate acts as owner for DDL; runtime never does.
GRANT aimee_kb_owner TO aimee_kb_migrate;
GRANT aimee_kb_status TO aimee_kb_status_login;
GRANT aimee_kb_token_roots_provision TO aimee_kb_migrate;
GRANT aimee_kb_jwks_publish TO aimee_kb_migrate;
GRANT aimee_kb_jwks_runtime_definer TO aimee_kb_migrate;
-- Unlike legacy definers, this online private-key compartment is not a migrate
-- membership.  Role/schema provisioning is performed by the database admin;
-- retaining membership would let the migration role execute the signing facade.
REVOKE aimee_kb_token_authority_definer FROM aimee_kb_migrate;
REVOKE aimee_kb_token_authority_store_owner FROM aimee_kb_migrate;

-- Schema usage: runtime may resolve objects but NOT create them.
GRANT USAGE ON SCHEMA public TO aimee_kb_runtime;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_runtime;
GRANT USAGE ON SCHEMA public TO aimee_kb_status;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_status;
GRANT USAGE ON SCHEMA public TO aimee_kb_status_login;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_status_login;
GRANT USAGE ON SCHEMA public TO aimee_kb_status_definer;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_status_definer;
GRANT USAGE ON SCHEMA public TO aimee_kb_token_roots_provision;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_token_roots_provision;
GRANT USAGE ON SCHEMA public TO aimee_kb_jwks_publish;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_jwks_publish;
GRANT USAGE ON SCHEMA public TO aimee_kb_jwks_runtime_definer;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_jwks_runtime_definer;
GRANT USAGE ON SCHEMA public TO aimee_kb_token_authority_definer;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_token_authority_definer;
GRANT USAGE ON SCHEMA public TO aimee_kb_token_authority_runtime;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_token_authority_runtime;
GRANT USAGE ON SCHEMA public TO aimee_kb_token_authority_store_owner;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_token_authority_store_owner;

-- NOTE: table/sequence/function GRANTs live in schema_grants.sql, applied AFTER
-- schema.sql (the tables must exist first). Provisioning order is:
--   1. schema_roles.sql  (this file — create roles + attributes)
--   2. schema.sql        (DDL, as the migrate/owner role)
--   3. schema_grants.sql  (runtime DML grants + WORM/function grants)
