#!/usr/bin/env bash
# Real-PostgreSQL proof for the SQLite WORM producer/worker bridge.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_URL="${1:-${AIMEE_TEST_PG_URL:-}}"
if [ -z "$BASE_URL" ]; then
  echo "run-worm-worker-pg-test: no Postgres URL (arg1 or AIMEE_TEST_PG_URL)." >&2
  exit 1
fi
ADMIN_URL="${BASE_URL%/*}/postgres"
TESTDB="aimee_worm_worker_gate_$$"
DB_URL="${BASE_URL%/*}/$TESTDB"
WORM_STATE="$(mktemp -d)"
WORM_DB="$WORM_STATE/audit/kb-worm-live.db"
cleanup() {
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" \
    -c "DROP DATABASE IF EXISTS $TESTDB WITH (FORCE)" >/dev/null 2>&1 || true
  rm -rf -- "$WORM_STATE"
}
trap cleanup EXIT HUP INT TERM

if [ ! -x "$ROOT/aimee-kb-worm" ]; then
  make -C "$ROOT/src" ../aimee-kb-worm >/dev/null
fi
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $TESTDB" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" \
  -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/src/modules/db2/c/schema_roles.sql" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" |
  psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/src/modules/db2/c/schema_grants.sql" >/dev/null

scalar() { psql -Atq -v ON_ERROR_STOP=1 "$DB_URL" -c "$1"; }
worm_scalar() { sqlite3 "$WORM_DB" "$1"; }
worker() {
  PGOPTIONS='-c role=aimee_kb_worm_worker' \
    AIMEE_HOME="$WORM_STATE" AIMEE_WORM_PATH="$WORM_DB" \
    AIMEE_WORM_DB2_URL="$DB_URL" "$ROOT/aimee-kb-worm" --once --batch=1000
}

psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<'SQL'
SET ROLE aimee_kb_worm_worker;
DO $$
BEGIN
  IF has_schema_privilege(current_user,'public','USAGE') OR
     NOT has_schema_privilege(current_user,'aimee_kb_worm_api','USAGE') OR
     NOT has_function_privilege(current_user,
       'aimee_kb_worm_api.claim(integer)','EXECUTE') OR
     NOT has_function_privilege(current_user,
       'aimee_kb_worm_api.ack(bigint,bigint)','EXECUTE') THEN
    RAISE EXCEPTION 'WORM worker schema capability is not isolated';
  END IF;
END $$;
RESET ROLE;
SQL
if PGOPTIONS='-c role=aimee_kb_worm_worker' psql -v ON_ERROR_STOP=1 "$DB_URL" \
  -c "SELECT public.pg_now_text()" >/dev/null 2>&1; then
  echo "WORM worker reached an unrelated public function" >&2
  exit 1
fi

psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<'SQL'
SET ROLE aimee_kb_runtime;
SELECT kb_audit_worm_submit('runtime','tester','worker.boundary','one','allow','');
DO $$
BEGIN
  IF has_table_privilege(current_user,'kb_audit_outbox','INSERT') OR
     has_table_privilege(current_user,'kb_audit_delivery','INSERT') OR
     has_schema_privilege(current_user,'aimee_kb_worm_api','USAGE') OR
     has_function_privilege(current_user,'kb_audit_worm_claim(integer)','EXECUTE') OR
     has_function_privilege(current_user,'kb_audit_worm_ack(bigint,bigint)','EXECUTE') THEN
    RAISE EXCEPTION 'runtime retained WORM worker privilege';
  END IF;
END $$;
RESET ROLE;
SQL
[ "$(scalar "SELECT count(*) FROM kb_audit_outbox")" = 1 ]
if AIMEE_HOME="$WORM_STATE" AIMEE_WORM_PATH="$WORM_DB" AIMEE_WORM_DB2_URL="$DB_URL" \
  "$ROOT/aimee-kb-worm" --once >/dev/null 2>&1; then
  echo "WORM worker accepted the producer/admin principal" >&2
  exit 1
fi
worker
[ "$(worm_scalar "SELECT count(*) FROM audit_event WHERE event_id<>'';")" = 1 ]
[ "$(scalar "SELECT count(*) FROM kb_audit_delivery")" = 1 ]

# Inject a failure after SQLite append but before PostgreSQL acknowledgement.
# The retry must reuse the existing stable event ID rather than duplicate it.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<'SQL'
SELECT kb_audit_worm_submit('runtime','tester','worker.retry','two','allow','');
SELECT kb_audit_worm_submit('runtime','tester','worker.retry','three','allow','');
CREATE OR REPLACE FUNCTION test_fail_delivery() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'injected delivery failure'; END $$;
CREATE TRIGGER aa_test_fail_delivery BEFORE INSERT ON kb_audit_delivery
  FOR EACH ROW EXECUTE FUNCTION test_fail_delivery();
SQL
before="$(worm_scalar "SELECT count(*) FROM audit_event WHERE event_id<>'';")"
if worker >/dev/null 2>&1; then
  echo "fault-injected WORM worker unexpectedly succeeded" >&2
  exit 1
fi
after_failure="$(worm_scalar "SELECT count(*) FROM audit_event WHERE event_id<>'';")"
[ "$after_failure" = "$((before + 1))" ] || {
  echo "fault injection did not reach the SQLite/ack boundary" >&2; exit 1; }
[ "$(scalar "SELECT pending_count FROM kb_audit_worm_pending()")" = 2 ]
psql -v ON_ERROR_STOP=1 "$DB_URL" \
  -c "DROP TRIGGER aa_test_fail_delivery ON kb_audit_delivery; DROP FUNCTION test_fail_delivery()" \
  >/dev/null
worker
[ "$(scalar "SELECT pending_count FROM kb_audit_worm_pending()")" = 0 ]
[ "$(worm_scalar "SELECT count(*) FROM audit_event WHERE event_id<>'';")" = 3 ]
[ "$(scalar "SELECT count(*) FROM kb_audit_delivery")" = 3 ]
[ "$(worm_scalar "SELECT count(*) FROM audit_event WHERE action='chain.checkpoint';")" -ge 1 ]

# Empty-queue restart is idempotent.
worker
[ "$(worm_scalar "SELECT count(*) FROM audit_event WHERE event_id<>'';")" = 3 ]

if psql -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "UPDATE kb_audit_outbox SET action='tampered' WHERE outbox_id=1" >/dev/null 2>&1; then
  echo "outbox UPDATE unexpectedly succeeded" >&2
  exit 1
fi
if psql -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "DELETE FROM kb_audit_delivery WHERE outbox_id=1" >/dev/null 2>&1; then
  echo "delivery DELETE unexpectedly succeeded" >&2
  exit 1
fi
broken="$(worm_scalar "SELECT count(*) FROM (SELECT seq,lag(row_hash) OVER (ORDER BY seq) p,prev_hash FROM audit_event) q WHERE seq>1 AND p<>prev_hash;")"
[ "$broken" = 0 ] || { echo "SQLite WORM chain has $broken broken links" >&2; exit 1; }

echo "SQLite WORM worker PostgreSQL bridge: PASSED"
echo "  producer: immutable outbox submit only"
echo "  worker: isolated claim/ack; crash retry is idempotent"
echo "  evidence: 3 delivered events in the shared SQLite chain, 0 broken links"
