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
# The seal fault-injection arm needs its own database: the schema it applies is
# deliberately broken, so it must never touch the one the real assertions run on.
UNSEALEDDB="aimee_fact_mutation_unsealed_$$"
UNSEALED_URL="${BASE_URL%/*}/$UNSEALEDDB"
WORK="$(mktemp -d)"
cleanup() {
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $TESTDB WITH (FORCE)" >/dev/null 2>&1 || true
  psql -v ON_ERROR_STOP=0 "$ADMIN_URL" -c "DROP DATABASE IF EXISTS $UNSEALEDDB WITH (FORCE)" >/dev/null 2>&1 || true
  rm -rf -- "$WORK"
}
trap cleanup EXIT HUP INT TERM

psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $TESTDB" >/dev/null
psql -v ON_ERROR_STOP=1 "$DB_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null

# Apply twice: the second pass is the production upgrade/idempotency contract.
for pass in 1 2; do
  sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" |
    psql -v ON_ERROR_STOP=1 "$DB_URL" -f - >/dev/null
done
psql -v ON_ERROR_STOP=1 "$DB_URL" -f "$ROOT/scripts/fact-mutation-pg-test.sql" >/dev/null

# Parallel producers write immutable outbox intents in their own transactions.
workers=8
for i in $(seq 1 "$workers"); do
  psql -v ON_ERROR_STOP=1 "$DB_URL" \
    -c "SELECT kb_audit_worm_append('test','concurrent:$i','fact.concurrent','$i','allow','')" \
    >"$WORK/$i.out" 2>"$WORK/$i.err" &
done
wait

pending="$(psql -Atq -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "SELECT pending_count FROM kb_audit_worm_pending()")"
[ "$pending" -ge "$workers" ] || {
  echo "expected at least $workers pending audit intents, got $pending" >&2; exit 1; }
count="$(psql -Atq -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "SELECT count(*) FROM kb_audit_outbox WHERE action='fact.concurrent'")"
[ "$count" = "$workers" ] || { echo "expected $workers concurrent audit intents, got $count" >&2; exit 1; }

# ---------------------------------------------------------------------------
# A memory mutation submits its changeset seal into the immutable WORM outbox in
# the same transaction. The separately privileged consumer appends it later.
#
# This drives evidence_object_mutation -- the trigger every direct write to
# memories/docs/document_versions goes through, and the path that closed a
# changeset leaving nothing in the audit chain before kb_fact_commit_worm_seal
# existed. The C mutation API's closes were always audited (fm_commit_finish);
# these SQL-side ones were not, and no unit test could see it: the sqlite shim
# carries the tables and the semantic guards but not this trigger, so the
# unsealed close does not exist to be observed until a real Postgres applies
# the real schema.
#
# set_config with is_local=false so the trigger reads the principal from the
# same session that performs the insert.
# ---------------------------------------------------------------------------
seal_probe() {
  local url="$1" key="$2"
  psql -v ON_ERROR_STOP=1 "$url" -c "
    SELECT set_config('aimee.principal','worm-seal-tester',false),
           set_config('aimee.authority','user',false);
    INSERT INTO memories(key,content) VALUES('$key','seal probe');" >/dev/null
}

# How many memory changesets closed with a WORM row carrying their commit_id.
sealed_count() {
  psql -Atq -v ON_ERROR_STOP=1 "$1" -c \
    "SELECT count(*) FROM fact_graph_commits c
      WHERE c.operation LIKE 'memory.%'
        AND EXISTS (SELECT 1 FROM kb_audit_outbox a
                     WHERE a.detail = 'commit_id=' || c.commit_id
                       AND a.action = c.operation)"
}

seal_probe "$DB_URL" "worm-seal-probe"
closed="$(psql -Atq -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "SELECT count(*) FROM fact_graph_commits WHERE operation LIKE 'memory.%' AND status='applied'")"
[ "$closed" -ge 1 ] || { echo "seal probe closed no memory changeset (got $closed)" >&2; exit 1; }

sealed="$(sealed_count "$DB_URL")"
[ "$sealed" = "$closed" ] || {
  echo "$closed memory changesets closed, only $sealed carry a WORM row" >&2; exit 1; }

# The seal must record the authenticated actor and authority off the changeset
# row, not whatever the caller felt like passing.
actor="$(psql -Atq -v ON_ERROR_STOP=1 "$DB_URL" -c \
  "SELECT a.actor_principal || '/' || a.actor_role FROM kb_audit_outbox a
     JOIN fact_graph_commits c ON a.detail = 'commit_id=' || c.commit_id
    WHERE c.operation LIKE 'memory.%' ORDER BY a.outbox_id DESC LIMIT 1")"
[ "$actor" = "worm-seal-tester/user" ] || {
  echo "seal recorded actor '$actor', expected 'worm-seal-tester/user'" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Negative control. Apply the same schema with the five seal CALLS stripped --
# the function itself stays, so this removes the wiring and nothing else -- and
# assert the probe above reports zero. Without this, the assertion could be
# passing on some other row that happens to carry a matching detail.
# ---------------------------------------------------------------------------
psql -v ON_ERROR_STOP=1 "$ADMIN_URL" -c "CREATE DATABASE $UNSEALEDDB" >/dev/null
psql -v ON_ERROR_STOP=1 "$UNSEALED_URL" -c "CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION IF NOT EXISTS pg_trgm" >/dev/null
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" |
  grep -v 'kb_fact_commit_worm_seal(cid' |
  psql -v ON_ERROR_STOP=1 "$UNSEALED_URL" -f - >/dev/null

seal_probe "$UNSEALED_URL" "worm-seal-probe-unsealed"
unsealed_closed="$(psql -Atq -v ON_ERROR_STOP=1 "$UNSEALED_URL" -c \
  "SELECT count(*) FROM fact_graph_commits WHERE operation LIKE 'memory.%' AND status='applied'")"
[ "$unsealed_closed" -ge 1 ] || {
  echo "negative control closed no changeset; it is not exercising the path" >&2; exit 1; }
unsealed_sealed="$(sealed_count "$UNSEALED_URL")"
[ "$unsealed_sealed" = "0" ] || {
  echo "negative control expected 0 sealed changesets, got $unsealed_sealed --" \
       "the seal assertion is passing on something other than the seal" >&2; exit 1; }

# The structural check that keeps a close added later from shipping unsealed,
# and its own self-test, so it cannot pass vacuously.
python3 "$ROOT/scripts/check_changeset_worm_seal.py" "$ROOT/src/modules/db2/c/schema.sql"
python3 "$ROOT/scripts/check_changeset_worm_seal.py" --self-test \
  "$ROOT/src/modules/db2/c/schema.sql"

echo "fact mutation PostgreSQL gate: PASSED"
echo "  memory changeset seal: $sealed of $closed closed changesets carry a WORM row"
echo "  negative control (seal calls stripped): $unsealed_sealed of $unsealed_closed"
