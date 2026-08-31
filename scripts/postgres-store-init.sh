#!/bin/bash
set -euo pipefail

: "${AIMEE_STORE_MIGRATOR_PASSWORD:?AIMEE_STORE_MIGRATOR_PASSWORD is required}"
: "${AIMEE_STORE_RUNTIME_PASSWORD:?AIMEE_STORE_RUNTIME_PASSWORD is required}"
: "${POSTGRES_PASSWORD:?POSTGRES_PASSWORD is required}"

# docker-entrypoint invokes this as POSTGRES_USER on a fresh cluster. The secure
# wrapper also invokes it against an existing pre-role-split cluster, where the
# historical superuser is `aimee`; in that case it supplies the discovered role
# explicitly. Keeping one idempotent reconciliation prevents fresh installs and
# upgrades from acquiring subtly different grants.
admin_user="${AIMEE_STORE_ADMIN_USER:-$POSTGRES_USER}"

psql --set=ON_ERROR_STOP=1 --username "$admin_user" --dbname "$POSTGRES_DB" \
  --set=admin_password="$POSTGRES_PASSWORD" \
  --set=migrator_password="$AIMEE_STORE_MIGRATOR_PASSWORD" \
  --set=runtime_password="$AIMEE_STORE_RUNTIME_PASSWORD" <<'SQL'
SELECT format('CREATE ROLE postgres LOGIN SUPERUSER PASSWORD %L', :'admin_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'postgres') \gexec
SELECT format('ALTER ROLE postgres WITH LOGIN SUPERUSER PASSWORD %L', :'admin_password') \gexec
SELECT format('CREATE ROLE aimee_store_migrator LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'migrator_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_store_migrator') \gexec
SELECT format('CREATE ROLE aimee_store_runtime LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'runtime_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_store_runtime') \gexec
SELECT format('ALTER ROLE aimee_store_migrator WITH LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'migrator_password') \gexec
SELECT format('ALTER ROLE aimee_store_runtime WITH LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'runtime_password') \gexec

ALTER DATABASE aimee_store OWNER TO aimee_store_migrator;
ALTER SCHEMA public OWNER TO aimee_store_migrator;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
REVOKE CONNECT ON DATABASE aimee_store FROM PUBLIC;
GRANT CONNECT ON DATABASE aimee_store TO aimee_store_runtime;
GRANT USAGE ON SCHEMA public TO aimee_store_runtime;

-- A fresh cluster has no application objects yet; an upgraded one does. Default
-- privileges only affect future migrations, so transfer and grant every
-- existing object before the old known-password owner is disabled.
DO $reconcile$
DECLARE
  object record;
  object_kind text;
BEGIN
  FOR object IN
    SELECT c.relkind, n.nspname, c.relname
      FROM pg_class c
      JOIN pg_namespace n ON n.oid = c.relnamespace
     WHERE n.nspname = 'public'
       AND c.relkind IN ('r', 'p', 'v', 'm', 'S', 'f')
       -- ALTER TABLE transfers its SERIAL/IDENTITY sequences atomically. A
       -- second ALTER SEQUENCE is rejected because an owned sequence may not
       -- have a different owner from its table; only standalone sequences
       -- need their own pass here.
       AND (c.relkind <> 'S' OR NOT EXISTS (
         SELECT 1 FROM pg_depend d
          WHERE d.objid = c.oid AND d.deptype IN ('a', 'i')
       ))
  LOOP
    object_kind := CASE object.relkind
      WHEN 'r' THEN 'TABLE'
      WHEN 'p' THEN 'TABLE'
      WHEN 'v' THEN 'VIEW'
      WHEN 'm' THEN 'MATERIALIZED VIEW'
      WHEN 'S' THEN 'SEQUENCE'
      WHEN 'f' THEN 'FOREIGN TABLE'
    END;
    EXECUTE format('ALTER %s %I.%I OWNER TO aimee_store_migrator',
                   object_kind, object.nspname, object.relname);
  END LOOP;
END
$reconcile$;

GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO aimee_store_runtime;
GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO aimee_store_runtime;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT EXECUTE ON FUNCTIONS TO aimee_store_runtime;
SQL
