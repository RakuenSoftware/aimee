#!/bin/bash
# Install and start the PostgreSQL transport on aimee-server's module bus.
# Run AS ROOT in the validation container.
set -u
CONF=/root
SOCK="$CONF/server-module-bus.sock"
BIN=/usr/local/libexec/aimee-modules/aimee-module-postgres
mkdir -p "$CONF/modules.d/server"

[ -x "$BIN" ] || cp -f /usr/local/libexec/aimee-modules/aimee-module-memory "$BIN" 2>/dev/null
[ -x "$BIN" ] || {
  echo "postgres: no module binary at $BIN" >&2
  exit 1
}

cat > "$CONF/modules.d/server/postgres.grant" <<'EOF'
version=1
principal_class=1
principal_ref=28
uid=self
executable=/usr/local/libexec/aimee-modules/aimee-module-postgres
publish=
subscribe=
request=
serve=11265,11266
EOF
echo "server postgres grant installed"

[ "${1:-both}" = "grants" ] && exit 0

for _ in $(seq 1 30); do [ -S "$SOCK" ] && break; sleep 1; done
[ -S "$SOCK" ] || { echo "server module bus socket missing" >&2; exit 1; }

pkill -f "aimee-module-postgres $SOCK" 2>/dev/null
sleep 1
cd /root
AIMEE_HOME="$CONF" \
AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_shared}" \
AIMEE_STORE_URL="${AIMEE_STORE_URL:-postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_shared}" \
  AIMEE_STORE_MIGRATION_URL="${AIMEE_STORE_MIGRATION_URL:-postgresql://aimee_migrator:aimee-migrate-e2e@127.0.0.1:5432/aimee_shared}" \
  nohup "$BIN" "$SOCK" >/root/postgres-module-server.log 2>&1 &
echo $! > /root/postgres-module-server.pid
sleep 4
if kill -0 "$(cat /root/postgres-module-server.pid)" 2>/dev/null; then
  echo "state: RUNNING"
else
  echo "state: EXITED"
  tail -5 /root/postgres-module-server.log
  exit 1
fi
