#!/usr/bin/env bash
# Validate the installed SQLite WORM paths on a fresh Debian guest.
set -euo pipefail

ROOT="${1:-/opt/aimee-worm-validation}"
SERVER_HOME=/var/lib/aimee-worm-server-validation
KB_HOME=/var/lib/aimee-worm-kb-validation
SERVER_PID=
CONFIG_PID=
SINGLETON_PID=
TEST_DB="aimee_worm_fresh_${RANDOM}_$$"

cleanup() {
  set +e
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -n "$CONFIG_PID" ]]; then
    kill "$CONFIG_PID" 2>/dev/null || true
    wait "$CONFIG_PID" 2>/dev/null || true
  fi
  if [[ -n "$SINGLETON_PID" ]]; then
    kill "$SINGLETON_PID" 2>/dev/null || true
    wait "$SINGLETON_PID" 2>/dev/null || true
  fi
  su postgres -c "psql -q -d postgres -c 'DROP DATABASE IF EXISTS $TEST_DB WITH (FORCE)'" \
    >/dev/null 2>&1 || true
  rm -f -- /tmp/aimee-worm-bridge-validation.log
  rm -rf -- "$SERVER_HOME" "$KB_HOME"
}
trap cleanup EXIT HUP INT TERM

pass=0
ok() {
  printf 'PASS  %s\n' "$1"
  pass=$((pass + 1))
}
fail() {
  printf 'FAIL  %s\n' "$*" >&2
  exit 1
}

install -m 0755 "$ROOT/aimee-server" /usr/local/bin/aimee-server
install -m 0755 "$ROOT/aimee-kb" /usr/local/bin/aimee-kb
install -m 0755 "$ROOT/aimee-kb-worm" /usr/local/bin/aimee-kb-worm
install -d -m 0755 /usr/local/libexec/aimee-modules
install -m 0755 "$ROOT/src/build/obj/aimee-module-config" \
  /usr/local/libexec/aimee-modules/aimee-module-config

ldd /usr/local/bin/aimee-server | grep -q 'libsqlite3' || fail 'server does not link SQLite'
if ldd /usr/local/bin/aimee-server | grep -q 'libpq'; then
  fail 'server unexpectedly links PostgreSQL'
fi
if ldd /usr/local/bin/aimee-kb | grep -q 'libsqlite3'; then
  fail 'KB process unexpectedly links SQLite'
fi
ldd /usr/local/bin/aimee-kb-worm | grep -q 'libsqlite3' || fail 'KB WORM worker lacks SQLite'
ldd /usr/local/bin/aimee-kb-worm | grep -q 'libpq' || fail 'KB WORM worker lacks libpq'
/usr/local/bin/aimee-server --version
/usr/local/bin/aimee-kb --version
ok 'installed binary ownership boundaries'

rm -rf -- "$SERVER_HOME"
install -d -m 0700 "$SERVER_HOME"
install -d -m 0700 "$SERVER_HOME/modules.d" "$SERVER_HOME/modules.d/server"
install -m 0600 "$ROOT/module-runtime/grants/server/config.grant" \
  "$SERVER_HOME/modules.d/server/config.grant"
export AIMEE_HOME="$SERVER_HOME"
SERVER_SOCKET="$SERVER_HOME/aimee-http.sock"
SERVER_BUS="$SERVER_HOME/server-module-bus.sock"
SERVER_DB="$SERVER_HOME/audit/worm-live.db"

start_server() {
  /usr/local/bin/aimee-server --foreground >"$SERVER_HOME/server.log" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 100); do
    [[ -S "$SERVER_BUS" ]] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
  done
  [[ -S "$SERVER_BUS" ]] || {
    tail -80 "$SERVER_HOME/server.log" >&2 || true
    fail 'aimee-server did not create its module bus'
  }
  AIMEE_HOME="$SERVER_HOME" /usr/local/libexec/aimee-modules/aimee-module-config \
    "$SERVER_BUS" >"$SERVER_HOME/config-module.log" 2>&1 &
  CONFIG_PID=$!
  for _ in $(seq 1 300); do
    [[ -S "$SERVER_SOCKET" ]] && return 0
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
  done
  tail -80 "$SERVER_HOME/server.log" >&2 || true
  fail 'aimee-server did not start on a fresh home'
}

stop_server() {
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
  fi
  if [[ -n "$CONFIG_PID" ]]; then
    kill "$CONFIG_PID" 2>/dev/null || true
    wait "$CONFIG_PID" 2>/dev/null || true
    CONFIG_PID=
  fi
}

server_api() {
  local method=$1 path=$2
  curl -fsS --unix-socket "$SERVER_SOCKET" -X "$method" "http://localhost$path"
}

start_server
snapshot="$(server_api POST /v1/audit/snapshot)"
grep -q '"snapshotted":true' <<<"$snapshot" || fail "server snapshot failed: $snapshot"
checkpoint="$(server_api POST /v1/audit/checkpoint)"
grep -q '"checkpointed":true' <<<"$checkpoint" || fail "server checkpoint failed: $checkpoint"
verify="$(server_api GET /v1/audit/verify)"
grep -q '"verify":"green"' <<<"$verify" || fail "server ledger not green: $verify"
[[ -s "$SERVER_DB" ]] || fail 'server did not create its SQLite WORM ledger'
server_count="$(sqlite3 "$SERVER_DB" 'SELECT count(*) FROM audit_event;')"
[[ "$server_count" -ge 2 ]] || fail 'server ledger did not record snapshot and checkpoint'
server_first_hash="$(sqlite3 "$SERVER_DB" 'SELECT row_hash FROM audit_event WHERE seq=1;')"
if sqlite3 "$SERVER_DB" "UPDATE audit_event SET detail='blocked' WHERE seq=1" >/dev/null 2>&1; then
  fail 'server SQLite append-only trigger allowed UPDATE'
fi
ok 'server fresh start, append-only write, checkpoint, and verification'

stop_server
start_server
verify="$(server_api GET /v1/audit/verify)"
grep -q '"verify":"red"' <<<"$verify" && fail "server restart broke verification: $verify"
[[ "$(sqlite3 "$SERVER_DB" 'SELECT row_hash FROM audit_event WHERE seq=1;')" = "$server_first_hash" ]] ||
  fail 'server restart changed the persisted chain prefix'
[[ "$(sqlite3 "$SERVER_DB" 'SELECT count(*) FROM audit_event;')" -ge "$server_count" ]] ||
  fail 'server restart lost persisted evidence'
checkpoint="$(server_api POST /v1/audit/checkpoint)"
grep -q '"checkpointed":true' <<<"$checkpoint" || fail "restart checkpoint failed: $checkpoint"
verify="$(server_api GET /v1/audit/verify)"
grep -q '"verify":"green"' <<<"$verify" || fail "restart checkpoint did not restore green: $verify"
ok 'server restart persistence and recovery'

# Simulate the immediately preceding SQLite schema: identical evidence rows but
# no event_id delivery column. Startup must migrate it additively without
# invalidating hashes or checkpoints.
stop_server
sqlite3 "$SERVER_DB" <<'SQL'
DROP INDEX audit_event_event_id;
DROP TRIGGER audit_event_no_update;
DROP TRIGGER audit_event_no_delete;
ALTER TABLE audit_event DROP COLUMN event_id;
CREATE TRIGGER audit_event_no_update BEFORE UPDATE ON audit_event
  BEGIN SELECT RAISE(ABORT, 'WORM: audit_event is append-only'); END;
CREATE TRIGGER audit_event_no_delete BEFORE DELETE ON audit_event
  BEGIN SELECT RAISE(ABORT, 'WORM: audit_event is append-only'); END;
SQL
start_server
verify="$(server_api GET /v1/audit/verify)"
grep -q '"verify":"red"' <<<"$verify" && fail "server upgrade invalidated ledger: $verify"
[[ "$(sqlite3 "$SERVER_DB" "SELECT count(*) FROM pragma_table_info('audit_event') WHERE name='event_id';")" = 1 ]] ||
  fail 'server did not add event_id during upgrade'
checkpoint="$(server_api POST /v1/audit/checkpoint)"
grep -q '"checkpointed":true' <<<"$checkpoint" || fail "upgrade checkpoint failed: $checkpoint"
verify="$(server_api GET /v1/audit/verify)"
grep -q '"verify":"green"' <<<"$verify" || fail "upgraded server ledger did not return green: $verify"
ok 'server additive SQLite schema upgrade preserves evidence'

stop_server
sqlite3 "$SERVER_DB" <<'SQL'
DROP TRIGGER audit_event_no_update;
UPDATE audit_event SET detail='offline-tamper' WHERE seq=1;
SQL
/usr/local/bin/aimee-server --foreground >"$SERVER_HOME/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 100); do
  kill -0 "$SERVER_PID" 2>/dev/null || break
  sleep 0.1
done
if kill -0 "$SERVER_PID" 2>/dev/null; then
  fail 'server remained running with an offline-tampered WORM ledger'
fi
if wait "$SERVER_PID"; then
  SERVER_PID=
  fail 'server returned success with an offline-tampered WORM ledger'
fi
SERVER_PID=
[[ ! -S "$SERVER_BUS" && ! -S "$SERVER_SOCKET" ]] ||
  fail 'server exposed a socket before rejecting the tampered WORM ledger'
grep -q 'SQLite WORM startup verification failed' "$SERVER_HOME/server.log" || {
  tail -80 "$SERVER_HOME/server.log" >&2 || true
  fail 'server did not identify WORM startup verification failure'
}
ok 'server refuses startup after offline SQLite tampering'

# Exercise the production PostgreSQL/SQLite bridge, including privilege
# isolation and the crash window after SQLite append but before PG ack.
BRIDGE_LOG=/tmp/aimee-worm-bridge-validation.log
if ! su postgres -c "bash '$ROOT/scripts/run-worm-worker-pg-test.sh' postgresql:///postgres" \
  >"$BRIDGE_LOG" 2>&1; then
  tail -120 "$BRIDGE_LOG" >&2 || true
  fail 'KB PostgreSQL/SQLite bridge failed'
fi
tail -4 "$BRIDGE_LOG"
ok 'KB PostgreSQL/SQLite bridge and idempotent crash retry'

rm -rf -- "$KB_HOME"
install -d -m 0700 -o postgres -g postgres "$KB_HOME" "$KB_HOME/audit"
DB_URL="postgresql:///$TEST_DB?host=/var/run/postgresql"
su postgres -c "psql -q -d postgres -c \"CREATE DATABASE $TEST_DB ENCODING 'UTF8' TEMPLATE template0\""
su postgres -c "psql -q -d '$TEST_DB' -c 'CREATE EXTENSION vector; CREATE EXTENSION pg_trgm;'"

# A prior PostgreSQL WORM evidence table is evidence, not migration trash. The
# new schema must leave it untouched while all new delivery goes to SQLite.
su postgres -c "psql -q -d '$TEST_DB' -c \
  \"CREATE TABLE kb_audit_event(legacy_marker text); INSERT INTO kb_audit_event VALUES ('retain-me');\""
su postgres -c "psql -q -d '$TEST_DB' -f '$ROOT/src/modules/db2/c/schema_roles.sql'"
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" |
  su postgres -c "psql -q -v ON_ERROR_STOP=1 -d '$TEST_DB'"
su postgres -c "psql -q -d '$TEST_DB' -f '$ROOT/src/modules/db2/c/schema_grants.sql'"
[[ "$(su postgres -c "psql -Atq -d '$TEST_DB' -c 'SELECT count(*) FROM kb_audit_event'")" = 1 ]] ||
  fail 'upgrade changed the legacy PostgreSQL evidence table'

su postgres -c "psql -q -d '$TEST_DB' -c \
  \"SET ROLE aimee_kb_runtime; SELECT kb_audit_worm_submit('runtime','fresh-guest','fresh.delivery','one','allow','');\""
run_worker() {
  su postgres -c "PGOPTIONS='-c role=aimee_kb_worm_worker' \
    AIMEE_HOME='$KB_HOME' AIMEE_WORM_PATH='$KB_HOME/audit/kb-worm-live.db' \
    AIMEE_WORM_DB2_URL='$DB_URL' /usr/local/bin/aimee-kb-worm --once --batch=100"
}
run_worker
[[ "$(sqlite3 "$KB_HOME/audit/kb-worm-live.db" "SELECT count(*) FROM audit_event WHERE event_id<>'';")" = 1 ]] ||
  fail 'KB worker did not persist the first SQLite event'
[[ "$(su postgres -c "psql -Atq -d '$TEST_DB' -c 'SELECT count(*) FROM kb_audit_delivery'")" = 1 ]] ||
  fail 'KB worker did not acknowledge the first delivery'

# Reapply the install schema as an upgrade and prove the legacy evidence and
# SQLite delivery are both unchanged.
sed 's/__EMBED_DIM__/1024/g' "$ROOT/src/modules/db2/c/schema.sql" |
  su postgres -c "psql -q -v ON_ERROR_STOP=1 -d '$TEST_DB'"
su postgres -c "psql -q -d '$TEST_DB' -f '$ROOT/src/modules/db2/c/schema_grants.sql'"
run_worker
[[ "$(su postgres -c "psql -Atq -d '$TEST_DB' -c 'SELECT count(*) FROM kb_audit_event'")" = 1 ]] ||
  fail 'schema reapply changed legacy PostgreSQL evidence'
[[ "$(sqlite3 "$KB_HOME/audit/kb-worm-live.db" "SELECT count(*) FROM audit_event WHERE event_id<>'';")" = 1 ]] ||
  fail 'empty-queue KB restart duplicated SQLite evidence'
ok 'KB upgrade preserves legacy PG evidence and SQLite delivery'

# A second logical worker must be refused by the PostgreSQL advisory lock.
su postgres -c "PGOPTIONS='-c role=aimee_kb_worm_worker' \
  AIMEE_HOME='$KB_HOME' AIMEE_WORM_PATH='$KB_HOME/audit/kb-worm-live.db' \
  AIMEE_WORM_DB2_URL='$DB_URL' /usr/local/bin/aimee-kb-worm --poll-ms=100" \
  >"$KB_HOME/singleton.log" 2>&1 &
SINGLETON_PID=$!
sleep 1
if run_worker >/dev/null 2>&1; then
  fail 'concurrent KB WORM worker was not refused'
fi
kill "$SINGLETON_PID" 2>/dev/null || true
wait "$SINGLETON_PID" 2>/dev/null || true
SINGLETON_PID=
ok 'KB advisory lock enforces one logical SQLite writer'

# Deliveries already exist. Replacing the store with an empty file must fail at
# ack rather than silently fork the evidence chain; restoring the real file must
# then drain the still-pending intent.
su postgres -c "psql -q -d '$TEST_DB' -c \
  \"SET ROLE aimee_kb_runtime; SELECT kb_audit_worm_submit('runtime','fresh-guest','fresh.delivery','two','allow','');\""
mv "$KB_HOME/audit/kb-worm-live.db" "$KB_HOME/audit/kb-worm-live.db.real"
if run_worker >/dev/null 2>&1; then
  fail 'KB worker accepted an empty replacement SQLite store'
fi
rm -f -- "$KB_HOME/audit/kb-worm-live.db" "$KB_HOME/audit/kb-worm-live.db-wal" \
  "$KB_HOME/audit/kb-worm-live.db-shm"
mv "$KB_HOME/audit/kb-worm-live.db.real" "$KB_HOME/audit/kb-worm-live.db"
chown postgres:postgres "$KB_HOME/audit/kb-worm-live.db"
run_worker
[[ "$(sqlite3 "$KB_HOME/audit/kb-worm-live.db" "SELECT count(*) FROM audit_event WHERE event_id<>'';")" = 2 ]] ||
  fail 'restored KB evidence chain did not drain pending intent'
ok 'KB stale/empty SQLite replacement fails closed'

# The worker verifies the full chain before claiming anything. An offline edit
# that bypasses triggers must therefore prevent startup.
sqlite3 "$KB_HOME/audit/kb-worm-live.db" <<'SQL'
DROP TRIGGER audit_event_no_update;
UPDATE audit_event SET detail='offline-tamper' WHERE event_id<>'' AND seq=(SELECT min(seq) FROM audit_event WHERE event_id<>'');
SQL
chown postgres:postgres "$KB_HOME/audit/kb-worm-live.db"
if run_worker >"$KB_HOME/tamper.log" 2>&1; then
  fail 'KB worker started against a tampered SQLite chain'
fi
grep -qiE 'verify|integrity|row_hash|RED' "$KB_HOME/tamper.log" || {
  cat "$KB_HOME/tamper.log" >&2
  fail 'KB worker failed after tamper without an integrity diagnostic'
}
ok 'KB worker refuses startup after offline SQLite tampering'

printf 'WORM fresh-guest validation: %d checks passed\n' "$pass"
