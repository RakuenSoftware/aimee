#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${AIMEE_TEST_PG_ADMIN:-}" ]]; then
  echo "AIMEE_TEST_PG_ADMIN is required (admin libpq URL ending in a maintenance database)" >&2
  exit 2
fi

bench_db="aimee_evidence_benchmark_${RANDOM}_$$"
schema_rendered="$(mktemp)"
cleanup() {
  rm -f "$schema_rendered"
  psql "$AIMEE_TEST_PG_ADMIN" -qAtc \
    "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='$bench_db' AND pid<>pg_backend_pid()" >/dev/null || true
  dropdb --if-exists --maintenance-db="$AIMEE_TEST_PG_ADMIN" "$bench_db" >/dev/null || true
}
trap cleanup EXIT

createdb --maintenance-db="$AIMEE_TEST_PG_ADMIN" "$bench_db"
admin_base="${AIMEE_TEST_PG_ADMIN%/*}"
bench_url="$admin_base/$bench_db"
psql "$bench_url" -v ON_ERROR_STOP=1 -q -c 'CREATE EXTENSION IF NOT EXISTS vector'
sed "s/__EMBED_DIM__/${AIMEE_TEST_EMBED_DIM:-768}/g" src/modules/db2/c/schema.sql >"$schema_rendered"
PGOPTIONS="--client-min-messages=warning" psql "$bench_url" -v ON_ERROR_STOP=1 -q -f "$schema_rendered"
PGOPTIONS="--client-min-messages=warning" psql "$bench_url" -v ON_ERROR_STOP=1 \
  -f scripts/evidence-lifecycle-benchmark.sql
