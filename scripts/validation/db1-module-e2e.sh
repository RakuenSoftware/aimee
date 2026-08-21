#!/bin/sh
# End-to-end verification of the DB1 module migration on a clean machine.
#
# This is not the repo's integration harness -- that runs against a build tree.
# This runs against an installed server, an installed module and an empty home,
# which is the shape a container deploy actually has, and it asserts the claims
# the migration makes rather than the ones a build tree can check:
#
#   * the daemon creates no database and never opens one
#   * the module creates the store and owns every table in it
#   * the daemon reports the store's health from the module's liveness
#   * sessions survive a module restart
#   * with the module gone the daemon degrades instead of lying or dying
#
# Every check prints PASS or FAIL and the run keeps going, so one failure does
# not hide the rest.

PASS=0
FAIL=0

ck() {
   _name=$1
   shift
   if "$@" >/dev/null 2>&1; then
      echo "PASS  $_name"
      PASS=$((PASS + 1))
   else
      echo "FAIL  $_name"
      FAIL=$((FAIL + 1))
   fi
}

ck_eq() {
   _name=$1
   _want=$2
   _got=$3
   if [ "$_want" = "$_got" ]; then
      echo "PASS  $_name"
      PASS=$((PASS + 1))
   else
      echo "FAIL  $_name (want '$_want', got '$_got')"
      FAIL=$((FAIL + 1))
   fi
}

ck_has() {
   _name=$1
   _hay=$2
   _needle=$3
   case "$_hay" in
   *"$_needle"*)
      echo "PASS  $_name"
      PASS=$((PASS + 1))
      ;;
   *)
      echo "FAIL  $_name (no '$_needle' in: $(printf '%s' "$_hay" | head -c 200))"
      FAIL=$((FAIL + 1))
      ;;
   esac
}

# pct exec hands over a minimal PATH that does not include /usr/local/bin,
# which is exactly where the deploy puts both binaries.
PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH

# Overridable so this can run against a build tree as well as an install.
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-db1}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/db1.grant}
HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-e2e-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SESSION_ID="e2e$$"
mkdir -p "$AIMEE_HOME/modules.d/server"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"

# The grant the supervisor would write. Copied from the build's generated grant
# so the served kinds cannot drift from what the module actually serves.
cp "$GRANT" "$AIMEE_HOME/modules.d/server/db1.grant"
SERVE=$(sed -n 's/^serve=//p' "$AIMEE_HOME/modules.d/server/db1.grant")
echo "grant serves: $SERVE"
echo

SERVER_PID=""
MODULE_PID=""

start_module() {
   AIMEE_DB1_PATH="$DB" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
   MODULE_PID=$!
   i=0
   while [ $i -lt 100 ]; do
      [ -S "$BUS_SOCK" ] && return 0
      kill -0 "$MODULE_PID" 2>/dev/null || return 1
      sleep 0.1
      i=$((i + 1))
   done
   return 1
}

# The module binding its socket is not the same event as the daemon having
# attached to it: the daemon dials on its own schedule. Waiting on the socket
# and then asserting readiness is a race, and the first run of this script lost
# it -- so wait for the thing actually being asserted.
wait_ready() {
   i=0
   while [ $i -lt 100 ]; do
      case "$(api GET /v1/server/health)" in
      *'"state":"ok"'*) return 0 ;;
      esac
      sleep 0.2
      i=$((i + 1))
   done
   return 1
}

stop_module() {
   [ -n "$MODULE_PID" ] || return 0
   kill "$MODULE_PID" 2>/dev/null
   wait "$MODULE_PID" 2>/dev/null
   MODULE_PID=""
}

start_server() {
   aimee-server --foreground >"$HOME/server.log" 2>&1 &
   SERVER_PID=$!
   i=0
   while [ $i -lt 300 ]; do
      [ -S "$HTTP_SOCK" ] && return 0
      kill -0 "$SERVER_PID" 2>/dev/null || return 1
      sleep 0.1
      i=$((i + 1))
   done
   return 1
}

api() {
   # $1 method, $2 path, $3 body (optional)
   if [ -n "$3" ]; then
      curl -s --unix-socket "$HTTP_SOCK" -X "$1" \
         -H 'Content-Type: application/json' -d "$3" "http://localhost$2"
   else
      curl -s --unix-socket "$HTTP_SOCK" -X "$1" "http://localhost$2"
   fi
}

echo "=============================================================="
echo " 1. the daemon starts against an empty home"
echo "=============================================================="
if start_server; then
   echo "PASS  server bound its socket on a home with no database"
   PASS=$((PASS + 1))
else
   echo "FAIL  server never bound; its output was:"
   tail -30 "$HOME/server.log" 2>/dev/null | sed 's/^/      /'
   FAIL=$((FAIL + 1))
   exit 1
fi

# The whole point of the migration: the daemon does not own the store. With no
# module attached there should be no database, because nothing in the daemon
# creates one any more.
if [ -e "$DB" ]; then
   echo "FAIL  daemon created a database on its own: $(ls -la "$DB")"
   FAIL=$((FAIL + 1))
else
   echo "PASS  daemon created no database"
   PASS=$((PASS + 1))
fi

# Stronger than "no file": the daemon holds no descriptor on any .db, so it
# could not be reading one that arrived by another route.
DBFDS=$(ls -l /proc/"$SERVER_PID"/fd 2>/dev/null | grep -c '\.db')
ck_eq "daemon holds no open database descriptor" "0" "$DBFDS"

echo
echo "=============================================================="
echo " 2. with no module attached the daemon degrades honestly"
echo "=============================================================="
HEALTH_NOMOD=$(api GET /v1/server/health)
ck_has "health reports the store unavailable" "$HEALTH_NOMOD" '"state":"unavailable"' 
case "$HEALTH_NOMOD" in
*'"state":"ok"'*)
   echo "FAIL  health claims the store is ok with no module attached"
   FAIL=$((FAIL + 1))
   ;;
*)
   echo "PASS  health does not claim ok with no module attached"
   PASS=$((PASS + 1))
   ;;
esac

# A store-backed call must fail, not fabricate. And the daemon must survive it.
CREATE_NOMOD=$(api POST /v1/sessions/create '{"title":"before module"}')
kill -0 "$SERVER_PID" 2>/dev/null
ck_eq "daemon still alive after a store call with no module" "0" "$?"
case "$CREATE_NOMOD" in
*'"session_id"'*)
   echo "FAIL  session create succeeded with no store: $CREATE_NOMOD"
   FAIL=$((FAIL + 1))
   ;;
*)
   echo "PASS  session create failed with no store"
   PASS=$((PASS + 1))
   ;;
esac

echo
echo "=============================================================="
echo " 3. the module creates and owns the store"
echo "=============================================================="
if start_module; then
   echo "PASS  module started and bound the bus socket"
   PASS=$((PASS + 1))
else
   echo "FAIL  module did not start; its output was:"
   tail -20 "$HOME/module.log" 2>/dev/null | sed 's/^/      /'
   FAIL=$((FAIL + 1))
fi

if wait_ready; then
   echo "PASS  daemon attached to the module"
   PASS=$((PASS + 1))
else
   echo "FAIL  daemon never reported the store ready after the module started"
   FAIL=$((FAIL + 1))
fi

ck "module created the database" test -s "$DB"
TABLES=$(sqlite3 "$DB" "select count(*) from sqlite_master where type='table'" 2>/dev/null)
echo "      tables in the module's store: ${TABLES:-0}"
if [ "${TABLES:-0}" -gt 50 ]; then
   echo "PASS  module created its full schema"
   PASS=$((PASS + 1))
else
   echo "FAIL  module schema looks short (${TABLES:-0} tables)"
   FAIL=$((FAIL + 1))
fi

# The module owns the file; the daemon still must not have opened it.
DBFDS2=$(ls -l /proc/"$SERVER_PID"/fd 2>/dev/null | grep -c '\.db')
ck_eq "daemon still holds no database descriptor" "0" "$DBFDS2"
MODFDS=$(ls -l /proc/"$MODULE_PID"/fd 2>/dev/null | grep -c 'aimee\.db')
if [ "${MODFDS:-0}" -ge 1 ]; then
   echo "PASS  the module is the process holding the store open"
   PASS=$((PASS + 1))
else
   echo "FAIL  module does not hold the store open"
   FAIL=$((FAIL + 1))
fi

echo
echo "=============================================================="
echo " 4. the daemon reports the store through the module"
echo "=============================================================="
HEALTH=$(api GET /v1/server/health)
ck_has "health reports the store reachable" "$HEALTH" '"state":"ok"'

echo
echo "=============================================================="
echo " 5. a session round-trips through the module"
echo "=============================================================="
CREATED=$(api POST /v1/sessions/create '{"title":"e2e round trip"}')
ck_has "session create succeeds" "$CREATED" '"session_id"'
SID=$(printf '%s' "$CREATED" | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
echo "      session id: ${SID:-<none>}"

LISTED=$(api POST /v1/sessions/list '{"limit":50}')
if [ -n "$SID" ]; then
   ck_has "created session comes back from list" "$LISTED" "$SID"
else
   echo "FAIL  no session id to look for"
   FAIL=$((FAIL + 1))
fi

# ... and it is genuinely in the module's file, not in daemon memory.
if [ -n "$SID" ]; then
   echo "      server_sessions columns: $(sqlite3 "$DB" \
      "select group_concat(name,',') from pragma_table_info('server_sessions')" 2>/dev/null)"
   ONDISK=$(sqlite3 "$DB" "select count(*) from server_sessions where id='$SID'" 2>/dev/null)
   [ "${ONDISK:-0}" = "1" ] || ONDISK=$(sqlite3 "$DB" \
      "select count(*) from server_sessions where session_id='$SID'" 2>/dev/null)
   ck_eq "session is on disk in the module's store" "1" "${ONDISK:-0}"
fi

echo
echo "=============================================================="
echo " 6. state survives a module restart"
echo "=============================================================="
stop_module
i=0
while [ $i -lt 120 ]; do
   case "$(api GET /v1/server/health)" in
   *'"state":"ok"'*) sleep 0.5 ;;
   *) break ;;
   esac
   i=$((i + 1))
done
echo "      health stopped reporting ok after ~$(echo "$i" | awk '{print $1*0.5}')s"
HEALTH_DOWN=$(api GET /v1/server/health)
case "$HEALTH_DOWN" in
*'"state":"ok"'*)
   echo "FAIL  health still claimed ok 60s after the module was killed"
   FAIL=$((FAIL + 1))
   ;;
*)
   echo "PASS  health stops claiming ok once the module is gone"
   PASS=$((PASS + 1))
   ;;
esac
kill -0 "$SERVER_PID" 2>/dev/null
ck_eq "daemon survived losing its module" "0" "$?"

if start_module; then
   echo "PASS  module restarted"
   PASS=$((PASS + 1))
else
   echo "FAIL  module did not restart"
   FAIL=$((FAIL + 1))
fi
wait_ready
HEALTH_BACK=$(api GET /v1/server/health)
ck_has "health recovers after the module returns" "$HEALTH_BACK" '"state":"ok"'
RELISTED=$(api POST /v1/sessions/list '{"limit":50}')
if [ -n "$SID" ]; then
   ck_has "the session is still there after the restart" "$RELISTED" "$SID"
fi

echo
echo "=============================================================="
echo " 7. exploratory: the CLI against a module-backed daemon"
echo "=============================================================="
CFG=$(aimee config get database 2>&1)
echo "      config get database: $(printf '%s' "$CFG" | head -c 120)"
SESS=$(aimee session list --limit 20 2>&1)
if [ -n "$SID" ]; then
   ck_has "cli session list shows the session" "$SESS" "$SID"
else
   echo "      cli session list: $(printf '%s' "$SESS" | head -c 160)"
fi
HEALTHCLI=$(aimee server health 2>&1 || true)
echo "      cli server health: $(printf '%s' "$HEALTHCLI" | head -c 200)"

echo
echo "      --- daemon log, anything that looks wrong ---"
grep -iE "error|fail|panic|assert|capability absent" "$HOME/server.log" 2>/dev/null \
   | grep -viE "no store|store unavailable" | head -15 | sed 's/^/      /'
echo "      --- module log ---"
tail -8 "$HOME/module.log" 2>/dev/null | sed 's/^/      /'

echo
echo "=============================================================="
echo " results: $PASS passed, $FAIL failed"
echo " home: $HOME"
echo "=============================================================="

stop_module
[ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
[ "$FAIL" -eq 0 ]
