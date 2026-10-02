#!/bin/bash
set -euo pipefail

: "${AIMEE_STORE_MIGRATOR_PASSWORD:?AIMEE_STORE_MIGRATOR_PASSWORD is required}"
: "${AIMEE_STORE_RUNTIME_PASSWORD:?AIMEE_STORE_RUNTIME_PASSWORD is required}"
: "${POSTGRES_PASSWORD:?POSTGRES_PASSWORD is required}"

# Provision the current administrator/migrator/runtime layout. Existing stores
# retain their application ownership and grants; domain migrations own those.
: "${POSTGRES_USER:?POSTGRES_USER is required}"
if [[ "$POSTGRES_USER" != postgres ]]; then
  echo "aimee store: POSTGRES_USER must be postgres" >&2
  exit 1
fi

psql --set=ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" \
  --set=admin_password="$POSTGRES_PASSWORD" \
  --set=migrator_password="$AIMEE_STORE_MIGRATOR_PASSWORD" \
  --set=runtime_password="$AIMEE_STORE_RUNTIME_PASSWORD" <<'SQL'
-- Extensions are owned by the PostgreSQL module, before restricted domain migrations.
SELECT format('CREATE EXTENSION IF NOT EXISTS %I', name)
FROM pg_available_extensions WHERE name IN ('vector','vectorscale','pg_trgm')
ORDER BY CASE name WHEN 'vector' THEN 1 WHEN 'vectorscale' THEN 2 ELSE 3 END \gexec
SELECT format('ALTER ROLE postgres WITH LOGIN SUPERUSER PASSWORD %L', :'admin_password') \gexec
SELECT format('CREATE ROLE aimee_store_migrator LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'migrator_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_store_migrator') \gexec
SELECT format('CREATE ROLE aimee_store_runtime LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'runtime_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_store_runtime') \gexec
SELECT format('ALTER ROLE aimee_store_migrator WITH LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'migrator_password') \gexec
SELECT format('ALTER ROLE aimee_store_runtime WITH LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'runtime_password') \gexec

-- Administrator-only worker provisioning precedes domain migrations.
\i /usr/local/share/aimee/postgres-hygiene-role.sql

ALTER DATABASE aimee_store OWNER TO aimee_store_migrator;
ALTER SCHEMA public OWNER TO aimee_store_migrator;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
REVOKE CONNECT ON DATABASE aimee_store FROM PUBLIC;
GRANT CONNECT ON DATABASE aimee_store TO aimee_store_runtime;
GRANT USAGE ON SCHEMA public TO aimee_store_runtime;

ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT EXECUTE ON FUNCTIONS TO aimee_store_runtime;

-- Preserve explicit protections when refreshing current role credentials.
-- Domain migrations apply the same restrictions when creating these tables.
DO $ledger$
BEGIN
  IF to_regclass('public.memory_rejection_tombstones') IS NOT NULL THEN
    REVOKE DELETE, TRUNCATE ON TABLE public.memory_rejection_tombstones FROM aimee_store_runtime, PUBLIC;
  END IF;
  IF to_regclass('public.schema_migrations') IS NOT NULL THEN
    REVOKE ALL ON TABLE public.schema_migrations FROM aimee_store_runtime, PUBLIC;
  END IF;
END
$ledger$;
SQL
