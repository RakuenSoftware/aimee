#!/bin/sh
# Upgrade path: an existing store, written by the pre-migration daemon, opened
# by the migrated build.
#
# Everything else validates a fresh home, where the module creates the schema
# itself. No real deployment is fresh. This one runs the OLD daemon first --
# which links db1_init and opens the database in-process, with only eight kinds
# served by its module -- writes real data through it, then stops it and brings
# up the NEW daemon and module on the same file.
#
# What has to hold afterwards: the data written by the old build is still there
# and still readable through the new one, the new module owns the file, the new
# daemon opens nothing, and writes keep working.
#
# Paths, all overridable:
#   OLD_SERVER  pre-migration aimee-server
#   OLD_MODULE  pre-migration aimee-module-db1
#   OLD_SERVE   its grant's serve list
#   NEW_SERVER / NEW_MODULE / NEW_SERVE  the migrated build
PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH
OLD_SERVER=${OLD_SERVER:-/opt/old/aimee-server}
OLD_MODULE=${OLD_MODULE:-/opt/old/aimee-module-db1}
OLD_SERVE=${OLD_SERVE:-11777,11778,11779,11780,11781,11782,11783,11784}
NEW_SERVER=${NEW_SERVER:-/usr/local/bin/aimee-server}
NEW_MODULE=${NEW_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-db1}
NEW_SERVE=${NEW_SERVE:-$(sed -n 's/^serve=//p' /opt/payload/grants/db1.grant 2>/dev/null)}

PASS=0
FAIL=0
say_pass() {
   echo "PASS  $1"
   PASS=$((PASS + 1))
}
say_fail() {
   echo "FAIL  $1"
   FAIL=$((FAIL + 1))
}

HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-upgrade-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SESSION_ID="upgrade$$"
mkdir -p "$AIMEE_HOME/modules.d/server"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"

write_grant() {
   cat >"$AIMEE_HOME/modules.d/server/db1.grant" <<GRANT
version=1
principal_class=1
principal_ref=30
uid=self
executable=$1
publish=
subscribe=
request=
serve=$2
GRANT
}

state() {
   curl -s --unix-socket "$HTTP_SOCK" http://localhost/v1/server/health |
      sed -n 's/.*"state":"\([a-z]*\)".*/\1/p'
}

create_session() {
   curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
      -d "{\"title\":\"$1\"}" http://localhost/v1/sessions/create
}

list_sessions() {
   curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
      -d '{"limit":100}' http://localhost/v1/sessions/list
}

boot() {
   # $1 server, $2 module, $3 log tag
   "$1" --foreground >"$HOME/server.$3.log" 2>&1 &
   SPID=$!
   i=0
   while [ $i -lt 300 ]; do
      [ -S "$HTTP_SOCK" ] && break
      kill -0 "$SPID" 2>/dev/null || return 1
      sleep 0.1
      i=$((i + 1))
   done
   AIMEE_DB1_PATH="$DB" "$2" "$BUS_SOCK" >"$HOME/module.$3.log" 2>&1 &
   MPID=$!
   i=0
   while [ $i -lt 150 ]; do
      [ "$(state)" = "ok" ] && return 0
      sleep 0.2
      i=$((i + 1))
   done
   return 1
}

halt() {
   [ -n "$MPID" ] && kill "$MPID" 2>/dev/null
   [ -n "$SPID" ] && kill "$SPID" 2>/dev/null
   wait "$MPID" 2>/dev/null
   wait "$SPID" 2>/dev/null
   MPID=""
   SPID=""
}

echo "=============================================================="
echo " 1. the pre-migration build writes a store"
echo "=============================================================="
write_grant "$OLD_MODULE" "$OLD_SERVE"
if boot "$OLD_SERVER" "$OLD_MODULE" old; then
   say_pass "old daemon and old module came up"
else
   say_fail "old build did not come up"
   tail -20 "$HOME/server.old.log" 2>/dev/null | sed 's/^/      /'
   exit 1
fi

OLD_IDS=""
n=1
while [ $n -le 4 ]; do
   r=$(create_session "written by the old build $n")
   id=$(printf '%s' "$r" | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
   printf '      create %s -> %s\n' "$n" "$(printf '%s' "$r" | head -c 160)"
   [ -n "$id" ] && OLD_IDS="$OLD_IDS $id"
   n=$((n + 1))
done
COUNT_OLD=$(printf '%s' "$OLD_IDS" | wc -w | tr -d ' ')
echo "      sessions written by the old build: $COUNT_OLD of 4"
# The old build reliably fails its FIRST store call after health has already
# gone green, then serves every one after it. That is the pre-migration build's
# behaviour and this script is not here to grade it -- it needs the old build to
# write a store, not to be correct. What matters is that the same gap does not
# exist in the migrated build, and that is asserted where it belongs: the e2e
# script's first store call comes straight after readiness and must succeed.
# So require enough rows to make the comparison meaningful, and no more.
[ "$COUNT_OLD" -ge 3 ] &&
   say_pass "the old build wrote a store worth upgrading ($COUNT_OLD sessions)" ||
   say_fail "the old build wrote only $COUNT_OLD sessions; too few to compare"

# The pre-migration daemon still opens the store itself. Recording that here is
# what makes the after-state meaningful rather than merely true.
OLDFDS=$(ls -l /proc/"$SPID"/fd 2>/dev/null | grep -c 'aimee\.db')
echo "      database descriptors held by the OLD daemon: $OLDFDS"

TABLES_OLD=$(sqlite3 "$DB" "select count(*) from sqlite_master where type='table'" 2>/dev/null)
ROWS_OLD=$(sqlite3 "$DB" "select count(*) from server_sessions" 2>/dev/null)
echo "      tables: ${TABLES_OLD:-0}, server_sessions rows: ${ROWS_OLD:-0}"

halt
sleep 1
say_pass "old build stopped, store left on disk"

echo
echo "=============================================================="
echo " 2. the migrated build opens that same store"
echo "=============================================================="
write_grant "$NEW_MODULE" "$NEW_SERVE"
if boot "$NEW_SERVER" "$NEW_MODULE" new; then
   say_pass "new daemon and new module came up on the existing store"
else
   say_fail "the migrated build did not come up on an existing store"
   echo "      --- server ---"
   tail -25 "$HOME/server.new.log" 2>/dev/null | sed 's/^/      /'
   echo "      --- module ---"
   tail -25 "$HOME/module.new.log" 2>/dev/null | sed 's/^/      /'
   exit 1
fi

TABLES_NEW=$(sqlite3 "$DB" "select count(*) from sqlite_master where type='table'" 2>/dev/null)
echo "      tables after the upgrade: ${TABLES_NEW:-0} (was ${TABLES_OLD:-0})"
if [ "${TABLES_NEW:-0}" -ge "${TABLES_OLD:-0}" ]; then
   say_pass "no table was dropped by the upgrade"
else
   say_fail "the upgrade lost tables: ${TABLES_OLD:-0} -> ${TABLES_NEW:-0}"
fi

echo
echo "=============================================================="
echo " 3. nothing the old build wrote was lost"
echo "=============================================================="
ROWS_NEW=$(sqlite3 "$DB" "select count(*) from server_sessions" 2>/dev/null)
[ "${ROWS_NEW:-0}" -ge "${ROWS_OLD:-0}" ] &&
   say_pass "every old row is still on disk (${ROWS_NEW:-0} >= ${ROWS_OLD:-0})" ||
   say_fail "rows disappeared: ${ROWS_OLD:-0} -> ${ROWS_NEW:-0}"

LISTED=$(list_sessions)
MISSING=0
for id in $OLD_IDS; do
   case "$LISTED" in
   *"$id"*) ;;
   *)
      MISSING=$((MISSING + 1))
      echo "      not returned by the new build: $id"
      ;;
   esac
done
[ "$MISSING" -eq 0 ] &&
   say_pass "the new build reads back every session the old one wrote" ||
   say_fail "$MISSING of $COUNT_OLD old sessions are unreadable through the new build"

echo
echo "=============================================================="
echo " 4. the migrated build behaves like the migrated build"
echo "=============================================================="
NEWFDS=$(ls -l /proc/"$SPID"/fd 2>/dev/null | grep -c '\.db')
[ "$NEWFDS" = "0" ] &&
   say_pass "the new daemon holds no database descriptor (the old one held $OLDFDS)" ||
   say_fail "the new daemon has $NEWFDS database descriptors open"

MODFDS=$(ls -l /proc/"$MPID"/fd 2>/dev/null | grep -c 'aimee\.db')
[ "${MODFDS:-0}" -ge 1 ] &&
   say_pass "the module is the process holding the upgraded store" ||
   say_fail "the module does not hold the store open"

AFTER=$(create_session "written after the upgrade")
case "$AFTER" in
*'"session_id"'*) say_pass "the upgraded store still takes writes" ;;
*) say_fail "write after upgrade failed: $AFTER" ;;
esac
NEWID=$(printf '%s' "$AFTER" | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
case "$(list_sessions)" in
*"$NEWID"*) say_pass "and reads the new row back beside the old ones" ;;
*) say_fail "the post-upgrade session did not come back from list" ;;
esac

echo
echo "      --- new module log ---"
tail -10 "$HOME/module.new.log" 2>/dev/null | sed 's/^/      /'
echo "      --- new daemon log, filtered ---"
grep -iE "capability absent|no such table|no such column|migrat|schema|SQL error" \
   "$HOME/server.new.log" 2>/dev/null | head -10 | sed 's/^/      /'

echo
echo "=============================================================="
echo " results: $PASS passed, $FAIL failed"
echo " home: $HOME"
echo "=============================================================="
halt
[ "$FAIL" -eq 0 ]
