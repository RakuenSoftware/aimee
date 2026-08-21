#!/bin/sh
# Exploratory sweep across the migrated families.
#
# The e2e script proves the sessions family round-trips. Sessions is one family
# of nineteen, and a migration that broke a different family would pass it. This
# drives the CLI across surfaces that reach the other eighteen and watches for
# the failures a bad cutover actually produces:
#
#   "capability absent"  -- an operation the module was supposed to serve
#   "internal error"     -- a stage that answered but answered wrong
#   a dead daemon or a dead module
#   a database descriptor appearing in the daemon
#
# A command failing for an unrelated reason (no model configured, kb offline) is
# not interesting and is not counted against the store. What is interesting is
# any sign that a call went to the module and did not come back correctly.
PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH
# Overridable so this can run against a build tree as well as an install.
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-db1}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/db1.grant}
HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-explore-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SESSION_ID="explore$$"
mkdir -p "$AIMEE_HOME/modules.d/server"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
cp "$GRANT" "$AIMEE_HOME/modules.d/server/db1.grant"

state() {
   curl -s --unix-socket "$HTTP_SOCK" http://localhost/v1/server/health |
      sed -n 's/.*"state":"\([a-z]*\)".*/\1/p'
}

aimee-server --foreground >"$HOME/server.log" 2>&1 &
SPID=$!
i=0
while [ $i -lt 300 ]; do [ -S "$HTTP_SOCK" ] && break; sleep 0.1; i=$((i + 1)); done
AIMEE_DB1_PATH="$DB" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
MPID=$!
i=0
while [ $i -lt 100 ]; do [ "$(state)" = "ok" ] && break; sleep 0.2; i=$((i + 1)); done
echo "server $SPID, module $MPID, store state: $(state)"
echo

SUSPECT=0
RAN=0
OUT="$HOME/sweep.log"
: >"$OUT"

try() {
   _family=$1
   shift
   RAN=$((RAN + 1))
   _res=$( ("$@") 2>&1 | head -c 1200 )
   printf '### [%s] %s\n%s\n\n' "$_family" "$*" "$_res" >>"$OUT"
   case "$_res" in
   *"capability absent"* | *"capability_absent"* | *"internal error"* | *"INTERNAL"* | \
      *"Segmentation"* | *"assertion"* | *"not initialized"* | *"database is locked"* | \
      *"no such table"* | *"no such column"* | *"SQL error"*)
      echo "SUSPECT  [$_family] $*"
      echo "         $(printf '%s' "$_res" | head -c 220)"
      SUSPECT=$((SUSPECT + 1))
      ;;
   *) echo "ok       [$_family] $*" ;;
   esac
}

echo "=== driving the CLI across the migrated families ==="
try sessions        aimee session list --limit 5
try sessions        aimee session list --json
try telemetry       aimee insights --days 7
try telemetry       aimee status
try telemetry       aimee workers
try economizer      aimee economizer
try runtime         aimee mcp list
try runtime         aimee toolset list
try runtime         aimee model list
try runtime         aimee provider list
try runtime         aimee catalog
try agent_work      aimee jobs
try agent_work      aimee job list
try agent_work      aimee cron list
try agent_work      aimee trigger list
try delegation      aimee episode list
try delegation      aimee delegate-backend
try workflow        aimee workflow list
try lifecycle       aimee workflow list --json
try ensemble        aimee ensemble
try roundtable      aimee roundtable
try roundtable      aimee pipeline
try guardrail_state aimee rules list
try guardrail_state aimee code
try conversation    aimee notes
try conversation    aimee memory list
try conversation    aimee wm list
try identity        aimee identity
try identity        aimee persona
try checkpoints     aimee trajectory
try checkpoints     aimee audit
try git_ownership   aimee repo
try pki             aimee cert
try vault           aimee vault list
try runtime         aimee api
try runtime         aimee dogfood
try runtime         aimee curator
try runtime         aimee workspace list
try runtime         aimee profile list
try runtime         aimee roles
try runtime         aimee aux
try runtime         aimee graph
try runtime         aimee hud

echo
echo "=== writes: does the store actually grow ==="
BEFORE=$(sqlite3 "$DB" "select sum(1) from sqlite_master where type='table'" 2>/dev/null)
for n in 1 2 3; do
   curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
      -d "{\"title\":\"sweep $n\"}" http://localhost/v1/sessions/create >/dev/null
done
ROWS=$(sqlite3 "$DB" "select count(*) from server_sessions" 2>/dev/null)
echo "  server_sessions rows after three creates: ${ROWS:-0}"
if [ "${ROWS:-0}" -ge 3 ]; then echo "  PASS  writes land in the module's store"
else echo "  FAIL  writes did not land"; SUSPECT=$((SUSPECT + 1)); fi

# Which tables the run actually touched, as a coarse check that more than one
# family did work.
echo
echo "=== non-empty tables in the module's store ==="
sqlite3 "$DB" "select name from sqlite_master where type='table' order by name" 2>/dev/null |
   while read -r t; do
      c=$(sqlite3 "$DB" "select count(*) from \"$t\"" 2>/dev/null)
      [ "${c:-0}" -gt 0 ] && printf '  %-34s %s\n' "$t" "$c"
   done

echo
echo "=== invariants after the sweep ==="
kill -0 "$SPID" 2>/dev/null && echo "  PASS  daemon still alive" || {
   echo "  FAIL  daemon died during the sweep"
   SUSPECT=$((SUSPECT + 1))
}
kill -0 "$MPID" 2>/dev/null && echo "  PASS  module still alive" || {
   echo "  FAIL  module died during the sweep"
   SUSPECT=$((SUSPECT + 1))
}
FDS=$(ls -l /proc/"$SPID"/fd 2>/dev/null | grep -c '\.db')
[ "$FDS" = "0" ] && echo "  PASS  daemon never opened a database" || {
   echo "  FAIL  daemon has $FDS database descriptors open"
   SUSPECT=$((SUSPECT + 1))
}
[ "$(state)" = "ok" ] && echo "  PASS  store still reports healthy" || {
   echo "  FAIL  store unhealthy after the sweep: $(state)"
   SUSPECT=$((SUSPECT + 1))
}

echo
echo "=== daemon log lines worth reading ==="
grep -iE "capability absent|internal|error|fail|assert|abort" "$HOME/server.log" 2>/dev/null |
   grep -viE "kb|unreachable|breaker|models\.dev|provider|no model|openrouter" |
   head -20 | sed 's/^/  /'
echo "=== module log ==="
tail -15 "$HOME/module.log" 2>/dev/null | sed 's/^/  /'

echo
echo "=============================================="
echo " $RAN commands driven, $SUSPECT suspect"
echo " full transcript: $OUT"
echo "=============================================="
kill "$MPID" "$SPID" 2>/dev/null
[ "$SUSPECT" -eq 0 ]
