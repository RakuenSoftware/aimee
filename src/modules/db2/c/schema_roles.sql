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
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_kb_worm_worker') THEN
    CREATE ROLE aimee_kb_worm_worker LOGIN NOINHERIT NOBYPASSRLS;
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
-- Dedicated online audit consumer. It receives no application-table or chain-
-- table grants: its only database capability is EXECUTE on the bounded drainer
-- installed by schema_grants.sql. Credentials are provisioned independently
-- from the ordinary runtime service.
ALTER ROLE aimee_kb_worm_worker LOGIN NOINHERIT NOBYPASSRLS NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
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

-- The WORM login is a terminal compartment, never a group and never a member
-- of another role. Repair accidental memberships in either direction so the
-- runtime cannot SET ROLE into the drainer and the worker cannot inherit an
-- application or owner capability.
DO $$
DECLARE edge RECORD;
BEGIN
  FOR edge IN
    SELECT granted.rolname AS granted_role, member.rolname AS member_role
      FROM pg_catalog.pg_auth_members am
      JOIN pg_catalog.pg_roles granted ON granted.oid=am.roleid
      JOIN pg_catalog.pg_roles member ON member.oid=am.member
     WHERE granted.rolname='aimee_kb_worm_worker'
        OR member.rolname='aimee_kb_worm_worker'
  LOOP
    EXECUTE format('REVOKE %I FROM %I CASCADE',edge.granted_role,edge.member_role);
  END LOOP;
END
$$;

-- The three token-authority roles are closed compartments, never grouping or
-- inheriting any other role and never inherited by any role.  Remove every
-- direct edge in either direction so reapplying provisioning also repairs an
-- accidental/operator-added membership.
DO $$
DECLARE edge RECORD;
BEGIN
  FOR edge IN
    SELECT granted.rolname AS granted_role, member.rolname AS member_role
      FROM pg_catalog.pg_auth_members am
      JOIN pg_catalog.pg_roles granted ON granted.oid=am.roleid
      JOIN pg_catalog.pg_roles member ON member.oid=am.member
     WHERE granted.rolname IN ('aimee_kb_token_authority_definer',
              'aimee_kb_token_authority_runtime','aimee_kb_token_authority_store_owner')
        OR member.rolname IN ('aimee_kb_token_authority_definer',
              'aimee_kb_token_authority_runtime','aimee_kb_token_authority_store_owner')
     ORDER BY CASE WHEN member.rolname IN ('aimee_kb_token_authority_definer',
              'aimee_kb_token_authority_runtime','aimee_kb_token_authority_store_owner')
              THEN 0 ELSE 1 END,granted.rolname,member.rolname
  LOOP
    -- CASCADE also removes memberships delegated by an authority role through
    -- ADMIN OPTION; a plain REVOKE can fail on or leave that dependent graph.
    EXECUTE format('REVOKE %I FROM %I CASCADE',edge.granted_role,edge.member_role);
  END LOOP;
END
$$;

-- Schema usage: runtime may resolve objects but NOT create them.
GRANT USAGE ON SCHEMA public TO aimee_kb_runtime;
REVOKE CREATE ON SCHEMA public FROM aimee_kb_runtime;
-- The WORM worker resolves only its dedicated API schema (created by
-- schema.sql). Deny public entirely so a future function's default EXECUTE
-- privilege cannot silently expand the worker's surface.
REVOKE ALL ON SCHEMA public FROM aimee_kb_worm_worker;
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
