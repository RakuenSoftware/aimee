#!/usr/bin/env bash
# Real-PostgreSQL migration, invariant, and concurrent WORM coverage for the
# authority-aware fact mutation seam.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "run-fact-mutation-pg-test: no Postgres URL (arg1 or AIMEE_TEST_PG_URL)." >&2
  exit 1
fi
ADMIN_URL="${BASE_URL%/*}/postgres"
TESTDB="aimee_fact_mutation_gate_$$"
DB_URL="${BASE_URL%/*}/$TESTDB"
WORK="$(mktemp -d)"
cleanup() {
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $TESTDB WITH (FORCE)" >/dev/null 2>&1 || true
  rm -rf -- "$WORK"
}
trap cleanup EXIT HUP INT TERM

psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $TESTDB" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null

# Apply twice: the second pass is the production upgrade/idempotency contract.
for pass in 1 2; do
  sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/db2/schema.sql" |
    psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null
done
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/fact-mutation-pg-test.sql" >/dev/null

# The SQL and C WORM appenders share this advisory-lock key. Parallel appenders
# must produce one gap-free chain rather than racing on seq/prev_hash.
workers=8
for i in $(seq 1 "$workers"); do
  psql -v ON_ERROR_STOP=1 "$DB_URL" \
    -c "SELECT kb_audit_worm_append('test','concurrent:$i','fact.concurrent','$i','allow','')" \
    >"$WORK/$i.out" 2>"$WORK/$i.err" &
done
wait

count="$(psql -Atq -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "SELECT count(*) FROM kb_audit_event WHERE action='fact.concurrent'")"
[ "$count" = "$workers" ] || { echo "expected $workers concurrent audit rows, got $count" >&2; exit 1; }
broken="$(psql -Atq -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "SELECT count(*) FROM (SELECT seq,lag(row_hash) OVER (ORDER BY seq) p,prev_hash FROM kb_audit_event) q WHERE seq>1 AND p<>prev_hash")"
[ "$broken" = "0" ] || { echo "concurrent WORM chain has $broken broken predecessor links" >&2; exit 1; }

echo "fact mutation PostgreSQL gate: PASSED"
