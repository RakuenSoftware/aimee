#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${AIMEE_TEST_PG_ADMIN:-}" ]]; then
  echo "AIMEE_TEST_PG_ADMIN is required (admin libpq URL ending in a maintenance database)" >&2
  exit 2
fi

db="aimee_evidence_lifecycle_${RANDOM}_$$"
schema_rendered="$(mktemp)"
export PGOPTIONS="${PGOPTIONS:---client-min-messages=warning}"
cleanup() {
  rm -f "$schema_rendered"
  psql "$AIMEE_TEST_PG_ADMIN" -v ON_ERROR_STOP=1 -qAtc \
    "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='$db' AND pid<>pg_backend_pid()" >/dev/null || true
  dropdb --if-exists --maintenance-db="$AIMEE_TEST_PG_ADMIN" "$db" >/dev/null || true
}
trap cleanup EXIT

createdb --maintenance-db="$AIMEE_TEST_PG_ADMIN" "$db"
admin_base="${AIMEE_TEST_PG_ADMIN%/*}"
db_url="$admin_base/$db"
psql "$db_url" -v ON_ERROR_STOP=1 -q -c 'CREATE EXTENSION IF NOT EXISTS vector'
sed "s/__EMBED_DIM__/${AIMEE_TEST_EMBED_DIM:-768}/g" \
  src/modules/db2/c/schema.sql >"$schema_rendered"
psql "$db_url" -v ON_ERROR_STOP=1 -q -f "$schema_rendered"
psql "$db_url" -v ON_ERROR_STOP=1 -f scripts/evidence-lifecycle-pg-test.sql
