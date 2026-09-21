#!/bin/bash
# Install and start the postgres module on aimee-kb's bus.
#
# The kb calls AIMEE_POSTGRES_EVENT_HEALTH, and the runtime bundle places the
# `postgres` module in kb alongside `memory` and `control-web`. Without it the
# health probe cannot answer, and the kb reports "DB2 schema not ready" -- while
# memory store/list/search work perfectly against the same database, because
# those go through the kb's own libpq path rather than the probe.
#
# That mismatch is worth naming: `aimee status` said the store was unavailable
# when it demonstrably was not, which is a false negative an operator would
# chase for a while.
# Run AS ROOT in the container.
set -u
CONF=/root/.config/aimee
SOCK="$CONF/kb-module-bus.sock"
BIN=/usr/local/libexec/aimee-modules/aimee-module-postgres
mkdir -p "$CONF/modules.d/kb"

# The BINARY must exist BEFORE the grant names it, and that includes `grants`
# mode. bus_runtime's parse_grant_file resolves `executable` with realpath(); a
# path that does not resolve makes the grant INVALID, and ONE invalid grant fails
# the whole module endpoint -- so aimee-kb refuses to start outright:
#
#   ERROR obs_bus: module grant policy is invalid: .../modules.d/kb
#   aimee-kb: module bus failed to start
#
# This was invisible on a container that had run before, because the binary was
# already there from an earlier pass. On a genuinely FRESH box the grant was
# written first and the copy sat after the grants-mode exit below, so the very
# first clean install could not bring the kb up at all. Found by provisioning a
# new container instead of reusing the one that had been running all session.
#
# aimee-module-postgres IS the memory binary: cmd/aimee-module picks its module
# from filepath.Base(argv[0]) minus the "aimee-module-" prefix.
[ -x "$BIN" ] || cp -f /usr/local/libexec/aimee-modules/aimee-module-memory "$BIN" 2>/dev/null
[ -x "$BIN" ] || {
  echo "postgres: no module binary at $BIN, and no memory binary to copy." >&2
  echo "          Refusing to write a grant that would stop the kb starting." >&2
  exit 1
}

cat > "$CONF/modules.d/kb/postgres.grant" <<'EOF'
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
echo "postgres grant installed"

# `grants` mode exists for the same reason config's and db1's do, and postgres
# needed it just as much: a daemon reads modules.d ONCE, at startup. Writing this
# grant after aimee-kb is already up means the kb never sees it, and the health
# call comes back AIMEE_MODULE_CALL_CAPABILITY_ABSENT -- with the module process
# running and looking healthy, which is what made `aimee status` report
# "store: unavailable" while the store worked.
[ "${1:-both}" = "grants" ] && exit 0

for _ in $(seq 1 30); do [ -S "$SOCK" ] && break; sleep 1; done
[ -S "$SOCK" ] || { echo "kb module bus socket missing" >&2; exit 1; }

pkill -f "aimee-module-postgres $SOCK" 2>/dev/null
sleep 1
cd /root
# The module opens its OWN connection: it is a separate process, so the kb's
# environment does not reach it. Without AIMEE_DB2_URL it answers "unset", the
# probe fails, and the kb reports "DB2 schema not ready" while the store works
# fine through the kb's own libpq path -- a false negative an operator would
# chase.
AIMEE_HOME="$CONF" \
AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_shared}" \
  AIMEE_STORE_URL="${AIMEE_STORE_URL:-postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_shared}" \
  AIMEE_STORE_MIGRATION_URL="${AIMEE_STORE_MIGRATION_URL:-postgresql://aimee_migrator:aimee-migrate-e2e@127.0.0.1:5432/aimee_shared}" \
  nohup /usr/local/libexec/aimee-modules/aimee-module-postgres "$SOCK" \
  >/root/postgres-module.log 2>&1 &
sleep 4
if pgrep -f "aimee-module-postgres $SOCK" >/dev/null; then
  echo "state: RUNNING"
else
  echo "state: EXITED"
  tail -3 /root/postgres-module.log
fi
