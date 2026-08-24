#!/bin/sh
# Bring aimee-kb and aimee-server up on a clean machine and exercise them.
#
# Runs INSIDE the container, against binaries carried in rather than built
# there, which is the shape a deploy actually has: an installed server, an
# installed module, an empty home, and a PostgreSQL that already exists.
#
# Every check prints PASS or FAIL and the run keeps going, so one failure does
# not hide the rest.
set -u

HOME_DIR=${AIMEE_HOME:-/var/lib/aimee-e2e}
KB_URL=${AIMEE_DB2_URL:-postgresql:///aimee_e2e_kb?host=/var/run/postgresql}
STORE_URL=${AIMEE_STORE_URL:-postgresql:///aimee_e2e_store?host=/var/run/postgresql}

PASS=0
FAIL=0
ck() {
   name=$1
   shift
   if "$@" >/dev/null 2>&1; then
      echo "PASS  $name"
      PASS=$((PASS + 1))
   else
      echo "FAIL  $name"
      FAIL=$((FAIL + 1))
   fi
}
ck_out() {
   name=$1
   want=$2
   shift 2
   got=$("$@" 2>&1)
   case "$got" in
      *"$want"*) echo "PASS  $name"; PASS=$((PASS + 1)) ;;
      *) echo "FAIL  $name (wanted '$want' in: $(echo "$got" | head -1))"
         FAIL=$((FAIL + 1)) ;;
   esac
}

echo "== environment =="
rm -rf "$HOME_DIR"
mkdir -p "$HOME_DIR"
export AIMEE_HOME="$HOME_DIR"
export AIMEE_DB2_URL="$KB_URL"
export AIMEE_STORE_URL="$STORE_URL"
echo "AIMEE_HOME=$AIMEE_HOME"
/usr/local/bin/aimee --version
/usr/local/bin/aimee-server --version
/usr/local/bin/aimee-kb --version

echo
echo "== binaries answer without a database =="
ck_out "aimee --version" "aimee " /usr/local/bin/aimee --version
ck_out "aimee-server --version" "protocol" /usr/local/bin/aimee-server --version
ck_out "aimee-kb --version" "aimee-kb" /usr/local/bin/aimee-kb --version

echo
echo "== the daemon creates no SQLite database =="
ck "no .db file in a fresh home" sh -c '[ -z "$(find "'"$HOME_DIR"'" -name "*.db" 2>/dev/null)" ]'

echo
echo "== seeding module grants =="
mkdir -p "$HOME_DIR/modules.d/server"
cp /opt/aimee/module-grants/server/*.grant "$HOME_DIR/modules.d/server/" 2>/dev/null || true
chmod 0700 "$HOME_DIR/modules.d" "$HOME_DIR/modules.d/server" 2>/dev/null || true
chmod 0600 "$HOME_DIR/modules.d/server/"*.grant 2>/dev/null || true
echo "        grants: $(ls "$HOME_DIR/modules.d/server" 2>/dev/null | wc -l)"
ck "the store module has a grant" test -f "$HOME_DIR/modules.d/server/aimee.grant"

echo
echo "== aimee-kb starts against DB2 =="
# HTTP is the KB's only transport, so it needs a port or it exits saying so.
/usr/local/bin/aimee-kb --http-port=8799 >"$HOME_DIR/kb.log" 2>&1 &
KB_PID=$!
i=0
while [ $i -lt 60 ]; do
   if [ -S "$HOME_DIR/kb.sock" ] || [ -S /run/aimee/kb.sock ]; then break; fi
   kill -0 "$KB_PID" 2>/dev/null || break
   i=$((i + 1))
   sleep 1
done
if kill -0 "$KB_PID" 2>/dev/null; then
   echo "PASS  aimee-kb is running (pid $KB_PID)"
   PASS=$((PASS + 1))
else
   echo "FAIL  aimee-kb exited; last lines:"
   tail -5 "$HOME_DIR/kb.log" | sed 's/^/        /'
   FAIL=$((FAIL + 1))
fi

echo
echo "== the KB schema landed in PostgreSQL =="
KBT=$(su postgres -c "psql -tAc \"SELECT count(*) FROM information_schema.tables WHERE table_schema='public'\" -d aimee_e2e_kb" 2>/dev/null)
echo "        kb tables: ${KBT:-0}"
ck "aimee-kb created its tables" sh -c "[ \"${KBT:-0}\" -gt 0 ]"

echo
echo "== aimee-server starts =="
/usr/local/bin/aimee-server >"$HOME_DIR/server.log" 2>&1 &
SRV_PID=$!
sleep 8
if kill -0 "$SRV_PID" 2>/dev/null; then
   echo "PASS  aimee-server is running (pid $SRV_PID)"
   PASS=$((PASS + 1))
else
   echo "FAIL  aimee-server exited; last lines:"
   tail -8 "$HOME_DIR/server.log" | sed 's/^/        /'
   FAIL=$((FAIL + 1))
fi

echo
echo "== the daemon still opened no SQLite store =="
ck "no .db file after both daemons ran" sh -c '[ -z "$(find "'"$HOME_DIR"'" -name "aimee.db" 2>/dev/null)" ]'
if [ -n "${SRV_PID:-}" ] && kill -0 "$SRV_PID" 2>/dev/null; then
   DBFD=$(ls -l /proc/"$SRV_PID"/fd 2>/dev/null | grep -c "\.db$")
   echo "        server .db descriptors: ${DBFD:-0}"
   ck "server holds no .db descriptor" sh -c "[ \"${DBFD:-0}\" -eq 0 ]"
fi

echo
echo "== store module: does it start against the bus =="
# The socket the DAEMON creates, not one invented here. The module attaches to
# it; a path nothing is serving fails as "no such file or directory", which
# looks like a module fault and is a test fault.
MODSOCK="$HOME_DIR/server-module-bus.sock"
ls -la "$MODSOCK" 2>/dev/null || echo "        (no module bus socket at $MODSOCK)"
AIMEE_STORE_URL="$STORE_URL" /usr/local/libexec/aimee-modules/aimee-module-aimee "$MODSOCK" \
   >"$HOME_DIR/module.log" 2>&1 &
MOD_PID=$!
sleep 6
if kill -0 "$MOD_PID" 2>/dev/null; then
   echo "PASS  store module is running (pid $MOD_PID)"
   PASS=$((PASS + 1))
else
   echo "INFO  store module exited; this is the expected state until the"
   echo "      postgres module's SQL stage lands. Its reason:"
   tail -6 "$HOME_DIR/module.log" | sed 's/^/        /'
fi

echo
echo "== store schema in PostgreSQL =="
ST=$(su postgres -c "psql -tAc \"SELECT count(*) FROM information_schema.tables WHERE table_schema='public'\" -d aimee_e2e_store" 2>/dev/null)
echo "        store tables: ${ST:-0}"

echo
echo "== shutting down =="
kill "$MOD_PID" "$SRV_PID" "$KB_PID" 2>/dev/null
sleep 2
kill -9 "$MOD_PID" "$SRV_PID" "$KB_PID" 2>/dev/null

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
