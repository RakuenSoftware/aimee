#!/bin/bash
set -euo pipefail

: "${AIMEE_STORE_MIGRATOR_PASSWORD:?AIMEE_STORE_MIGRATOR_PASSWORD is required}"
: "${AIMEE_STORE_RUNTIME_PASSWORD:?AIMEE_STORE_RUNTIME_PASSWORD is required}"

psql --set=ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" \
  --set=migrator_password="$AIMEE_STORE_MIGRATOR_PASSWORD" \
  --set=runtime_password="$AIMEE_STORE_RUNTIME_PASSWORD" <<'SQL'
SELECT format('CREATE ROLE aimee_store_migrator LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'migrator_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_store_migrator') \gexec
SELECT format('CREATE ROLE aimee_store_runtime LOGIN PASSWORD %L NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION', :'runtime_password')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'aimee_store_runtime') \gexec

ALTER DATABASE aimee_store OWNER TO aimee_store_migrator;
ALTER SCHEMA public OWNER TO aimee_store_migrator;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
GRANT CONNECT ON DATABASE aimee_store TO aimee_store_runtime;
GRANT USAGE ON SCHEMA public TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO aimee_store_runtime;
ALTER DEFAULT PRIVILEGES FOR ROLE aimee_store_migrator IN SCHEMA public
  GRANT EXECUTE ON FUNCTIONS TO aimee_store_runtime;
SQL
