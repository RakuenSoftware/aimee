#!/usr/bin/env bash
# run-p1-rls-gate.sh: the mandatory P1 DB-layer tenancy-isolation gate.
#
# Provisions a throwaway database on the target Postgres with the three-role split
# (schema_roles.sql) + the full aimee schema (schema.sql), then runs the RLS
# isolation assertions (p1_rls_isolation_test.sql). Any failure exits non-zero, so
# this is a HARD CI gate — it does not skip. Requires a real Postgres with the
# pgvector + pg_trgm extensions (the SQLite shim cannot enforce RLS).
#
# Connection: pass a libpq base URL as $1, or set PG* env. The connecting role must
# be a superuser (needs CREATE DATABASE / EXTENSION / SET ROLE) — CI's pgvector
# sidecar 'aimee' user is one. Example:
#   scripts/run-p1-rls-gate.sh postgres://aimee:aimee@localhost:5432/postgres
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "run-p1-rls-gate: no Postgres URL (arg1 or AIMEE_TEST_PG_URL). This gate does not skip." >&2
  exit 1
fi

# Admin URL points at the maintenance db; derive one on the same server.
ADMIN_URL="${BASE_URL%/*}/postgres"
TESTDB="aimee_p1_rls_gate"

echo "== P1 RLS gate: provisioning $TESTDB =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $TESTDB;"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $TESTDB;"
DB_URL="${BASE_URL%/*}/$TESTDB"

psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;"

# Three-phase provisioning, matching the real hardened deploy order:
#   1. roles (create) -> 2. schema (DDL) -> 3. grants (runtime DML/EXECUTE).
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/src/db2/schema_roles.sql"
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/db2/schema.sql" | psql -v ON_ERROR_STOP=1 "$DB_URL" -f -
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/src/db2/schema_grants.sql"

echo "== P1 RLS gate: running isolation assertions =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p1_rls_isolation_test.sql"

echo "== P1 RLS gate: cleanup =="
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $TESTDB;"
echo "== P1 RLS gate: PASSED =="
