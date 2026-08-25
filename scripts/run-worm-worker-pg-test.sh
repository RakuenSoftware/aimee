#!/usr/bin/env bash
# Real-PostgreSQL proof for the WORM producer/worker privilege and recovery split.
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
cleanup() {
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" \
    -c "DROP DATABASE IF EXISTS $TESTDB WITH (FORCE)" >/dev/null 2>&1 || true
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
worker() {
  PGOPTIONS='-c role=aimee_kb_worm_worker' \
    AIMEE_WORM_DB2_URL="$DB_URL" "$ROOT/aimee-kb-worm" --once --batch=1000
}

# The worker can resolve only its one-function API schema. In particular it
# cannot exploit a future application function that retains PostgreSQL's
# default PUBLIC EXECUTE grant, because it cannot resolve schema public at all.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<'SQL'
SET ROLE aimee_kb_worm_worker;
DO $$
BEGIN
  IF has_schema_privilege(current_user,'public','USAGE') OR
     NOT has_schema_privilege(current_user,'aimee_kb_worm_api','USAGE') OR
     NOT has_function_privilege(current_user,
       'aimee_kb_worm_api.drain(integer)','EXECUTE') THEN
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

# Runtime can submit, but it cannot touch or drain any audit storage directly.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<'SQL'
SET ROLE aimee_kb_runtime;
SELECT kb_audit_worm_submit('runtime','tester','worker.boundary','one','allow','');
DO $$
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_auth_members m
      JOIN pg_roles granted ON granted.oid=m.roleid
      JOIN pg_roles member ON member.oid=m.member
     WHERE granted.rolname='aimee_kb_worm_worker'
        OR member.rolname='aimee_kb_worm_worker') THEN
    RAISE EXCEPTION 'WORM worker participates in a role-membership edge';
  END IF;
  IF has_table_privilege(current_user,'kb_audit_event','INSERT') OR
     has_table_privilege(current_user,'kb_audit_outbox','INSERT') OR
     has_table_privilege(current_user,'kb_audit_delivery','INSERT') OR
     has_schema_privilege(current_user,'aimee_kb_worm_api','USAGE') OR
     has_function_privilege(current_user,'kb_audit_worm_drain(integer)','EXECUTE') OR
     has_function_privilege(current_user,
       'kb_audit_worm_append_internal(text,text,text,text,text,text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'runtime retained WORM writer privilege';
  END IF;
  IF NOT has_function_privilege(current_user,
       'kb_audit_worm_submit(text,text,text,text,text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'runtime cannot submit durable audit intents';
  END IF;
END $$;
RESET ROLE;
SQL
[ "$(scalar "SELECT count(*) FROM kb_audit_outbox")" = 1 ]
[ "$(scalar "SELECT count(*) FROM kb_audit_event")" = 0 ]
if AIMEE_WORM_DB2_URL="$DB_URL" "$ROOT/aimee-kb-worm" --once >/dev/null 2>&1; then
  echo "WORM worker accepted the producer/admin database principal" >&2
  exit 1
fi
worker
[ "$(scalar "SELECT count(*) FROM kb_audit_event")" = 1 ]
[ "$(scalar "SELECT count(*) FROM kb_audit_delivery")" = 1 ]

# Fail after the chain INSERT but before delivery acknowledgement. Because the
# drain call is one transaction, both writes must roll back and all intents must
# remain pending for restart recovery.
psql -v ON_ERROR_STOP=1 "$DB_URL" >/dev/null <<'SQL'
SELECT kb_audit_worm_submit('runtime','tester','worker.retry','two','allow','');
SELECT kb_audit_worm_submit('runtime','tester','worker.retry','three','allow','');
CREATE OR REPLACE FUNCTION test_fail_delivery() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'injected delivery failure'; END $$;
CREATE TRIGGER aa_test_fail_delivery BEFORE INSERT ON kb_audit_delivery
  FOR EACH ROW EXECUTE FUNCTION test_fail_delivery();
SQL
before="$(scalar "SELECT count(*) FROM kb_audit_event")"
if worker >/dev/null 2>&1; then
  echo "fault-injected WORM worker unexpectedly succeeded" >&2
  exit 1
fi
[ "$(scalar "SELECT count(*) FROM kb_audit_event")" = "$before" ] || {
  echo "failed drain left a partial chain append" >&2; exit 1; }
[ "$(scalar "SELECT pending_count FROM kb_audit_worm_pending()")" = 2 ]
psql -v ON_ERROR_STOP=1 "$DB_URL" \
  -c "DROP TRIGGER aa_test_fail_delivery ON kb_audit_delivery; DROP FUNCTION test_fail_delivery()" \
  >/dev/null
worker
[ "$(scalar "SELECT pending_count FROM kb_audit_worm_pending()")" = 0 ]
[ "$(scalar "SELECT count(*) FROM kb_audit_event")" = 3 ]
[ "$(scalar "SELECT count(*) FROM kb_audit_delivery")" = 3 ]
[ "$(scalar "SELECT count(*) FROM kb_vault_witness_log WHERE tenant='!kb' AND provider='!audit'")" = 3 ]

# A restart over an empty queue is idempotent.
worker
[ "$(scalar "SELECT count(*) FROM kb_audit_event")" = 3 ]

# Even the schema owner cannot mutate queue history without disabling the WORM
# triggers; ordinary producer and worker roles have no table privileges at all.
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

broken="$(scalar "SELECT count(*) FROM (SELECT seq,lag(row_hash) OVER (ORDER BY seq) p,prev_hash FROM kb_audit_event) q WHERE seq>1 AND p<>prev_hash")"
[ "$broken" = 0 ] || { echo "WORM chain has $broken broken links" >&2; exit 1; }

echo "WORM worker PostgreSQL gate: PASSED"
echo "  producer: submit only; chain/outbox/delivery writes denied"
echo "  worker: bounded drain only; crash rollback and retry verified"
echo "  chain: 3 rows + 3 witnesses, idempotent restart, 0 broken links"
