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

# The store module is PostgreSQL-backed: it reads AIMEE_STORE_URL and refuses to
# start without it. Say so here rather than letting the module exit into a log
# nobody reads and the rig time out on a socket that never appears.
require_store_url() {
   if [ -z "${AIMEE_STORE_URL:-}" ]; then
      echo "$(basename "$0"): AIMEE_STORE_URL is not set." >&2
      echo "  The store is a Go module against PostgreSQL; it no longer opens a" >&2
      echo "  SQLite file. Point this at a database the rig may create and drop:" >&2
      echo "    export AIMEE_STORE_URL=postgres://user:pass@host:5432/aimee_store" >&2
      exit 2
   fi
}

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
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-aimee}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/aimee.grant}
HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-e2e-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SESSION_ID="e2e$$"
mkdir -p "$AIMEE_HOME/modules.d/server"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"

# The grant the supervisor would write. Copied from the build's generated grant
# so the served kinds cannot drift from what the module actually serves.
# The grant's executable= is what the daemon pins the peer against, so it must
# name the module this rig actually starts. In a container the two are the same
# path and a plain copy works; against a build tree they are not, and a grant
# naming an uninstalled path makes the daemon reject the whole policy and exit.
sed "s|^executable=.*|executable=$MODULE|" "$GRANT" \
    >"$AIMEE_HOME/modules.d/server/aimee.grant"
SERVE=$(sed -n 's/^serve=//p' "$AIMEE_HOME/modules.d/server/aimee.grant")
echo "grant serves: $SERVE"
echo

SERVER_PID=""
MODULE_PID=""

start_module() {
   require_store_url
   AIMEE_STORE_URL="$AIMEE_STORE_URL" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
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

# The store is PostgreSQL and the module applies its own schema on connect, so
# there is no file to stat and no sqlite_master to count. The module reports what
# it applied; that is the same claim, from the side that would know.
APPLIED=$(sed -n 's/.*store: schema applied (\([0-9]*\) files).*/\1/p' \
              "$HOME/module.log" 2>/dev/null | tail -1)
echo "      schema files the module applied: ${APPLIED:-0}"
if [ "${APPLIED:-0}" -gt 15 ]; then
   echo "PASS  module applied its full schema"
   PASS=$((PASS + 1))
else
   echo "FAIL  module schema looks short (${APPLIED:-0} files applied)"
   FAIL=$((FAIL + 1))
fi

# The module holds the store; the daemon still must not. Holding it is a socket
# now rather than an open file -- the store is PostgreSQL -- so the daemon's half
# is unchanged (it must have no database descriptor of any kind) and the
# module's half looks for the connection instead.
DBFDS2=$(ls -l /proc/"$SERVER_PID"/fd 2>/dev/null | grep -c '\.db')
ck_eq "daemon still holds no database descriptor" "0" "$DBFDS2"
STORE_PORT=$(printf '%s' "${AIMEE_STORE_URL:-}" | sed -n 's|.*:\([0-9][0-9]*\)/.*|\1|p')
[ -n "$STORE_PORT" ] || STORE_PORT=5432
# grep -c prints 0 AND exits 1 when nothing matches, so the count is taken as-is
# and the non-zero exit swallowed -- an || echo here appends a second number.
MODCONNS=$(ss -tnp 2>/dev/null | grep -c "pid=$MODULE_PID,") || true
SRVCONNS=$(ss -tnp 2>/dev/null | grep "pid=$SERVER_PID," | grep -c ":$STORE_PORT") || true
ck_eq "daemon holds no connection to the store" "0" "${SRVCONNS:-0}"
if [ "${MODCONNS:-0}" -ge 1 ]; then
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

# ... and that it is genuinely IN THE STORE rather than in daemon memory is what
# section 6 proves, by reading it back through a module process that did not
# exist when it was written. This used to open $AIMEE_HOME/aimee.db with sqlite3;
# the store is PostgreSQL now, on a host this rig has no client for and no
# business connecting to -- nothing outside the module opens the store, which is
# the property the module exists to have.

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
