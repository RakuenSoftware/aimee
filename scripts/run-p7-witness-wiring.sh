#!/usr/bin/env bash
# P7-witness-e2 wiring gate: provisions a CLEAN isolated database and proves the
# atomic witness append is invoked from the reseal and open source ledgers
# inside their own transactions, with content-binding source hashes and
# idempotent replay. Kept OUT of run-p1-rls-gate.sh because it drives real vault
# functions and mutates kb_vault_control, which the shared RLS-gate DB's other P7
# tests also touch. The connecting role must be a superuser.
#   scripts/run-p7-witness-wiring.sh postgres://aimee:aimee@localhost:5432/postgres
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "usage: $0 <superuser libpq url>   (or set AIMEE_TEST_PG_URL)" >&2
  exit 2
fi
ADMIN_URL="${BASE_URL%/*}/postgres"
DB="aimee_p7_witness_wiring_gate"

psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $DB;"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $DB;"
DB_URL="${BASE_URL%/*}/$DB"
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null

echo "== P7-witness-e2 wiring: independent reseal + open witness ledgers =="
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/p7_witness_wiring_pg_test.sql"

psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE $DB;"
echo "== P7-witness-e2 wiring gate: PASSED =="

# Atomicity gets its OWN database: the wiring test deliberately forges a shard to
# prove the cross-check catches it, which would poison every later assertion.
ADB="aimee_p7_witness_atomicity_gate"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $ADB;"
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $ADB;"
ADB_URL="${BASE_URL%/*}/$ADB"
psql -v ON_ERROR_STOP=1 "$ADB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm;" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" | psql -v ON_ERROR_STOP=1 "$ADB_URL" -f - >/dev/null

echo "== P7-witness-e3 source-plus-witness atomicity =="
psql -v ON_ERROR_STOP=1 "$ADB_URL" -f "$ROOT/scripts/p7_witness_atomicity_pg_test.sql"

psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "DROP DATABASE $ADB;"
echo "== P7-witness-e3 atomicity gate: PASSED =="
