#!/bin/sh
# The topology that actually ships: C daemon + DB1 module + the Go WFE.
#
# Every other validation script here runs the daemon and the module and nothing
# else, and concludes that the module owns the store because the daemon holds no
# descriptor on it. That conclusion is about a two-process system. The container
# runs three: server-entrypoint.sh defaults AIMEE_WFE_ENGINE=go and launches
# /usr/local/bin/aimee-wfe with --home and no --db, and cmd/aimee-server then
# does
#
#     *dbPath = filepath.Join(*home, "aimee.db")
#
# and opens it with sql.Open. So a third process opens the same file the module
# owns -- and it does not merely read it: internal/db1/store.go has its own
# CREATE TABLE IF NOT EXISTS and its own ALTER TABLE ladder over
# lifecycle_work_item, lifecycle_event, lifecycle_stage_attempt,
# lifecycle_delegate_job, agent_jobs, wfe_convergence and wfe_frozen_create --
# tables the module's lifecycle and delegation families now serve.
#
# This script does not assume that is broken. It measures it:
#
#   * how many processes hold aimee.db open once all three are up
#   * whether the Go side changes the schema the module created
#   * whether concurrent writes from both sides produce lock failures
#
# This script was written to FAIL. It asserted the state the doctrine asks for --
# one owner of the store -- against an appliance that had two, and its failures
# were the finding rather than a broken test. On a clean container it reported:
# two holders of aimee.db, five columns added to lifecycle_work_item by the Go
# side, and seven tables created by the Go WFE when it started first.
#
# It passes now. The engine reaches DB1 through the module, so there is one
# holder, nothing for a second writer to alter, and nothing for it to create.
# The script stays exactly as it was, because an assertion that has started
# passing is worth more than one written after the fact -- it is the same
# measurement, and it is what would notice the gap reopening.
#
# Overridable: MODULE, GRANT, SERVER, WFE.
PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-db1}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/db1.grant}
# The container grants the Go WFE bus access too (principal_ref 64). Without it
# the WFE exits on "bus: attach denied" -- but only AFTER it has opened the
# store and run its migrations, which is itself worth knowing and was how this
# was first seen. Install it so the WFE stays up and the coexistence being
# measured is the sustained one the container actually runs.
WFE_GRANT=${AIMEE_WFE_GRANT:-/opt/payload/grants/wfe.grant}
SERVER=${AIMEE_SERVER_BIN:-/usr/local/bin/aimee-server}
WFE=${AIMEE_WFE_BIN:-/usr/local/bin/aimee-wfe}

PASS=0
FAIL=0
NOTE=0
say_pass() {
   echo "PASS  $1"
   PASS=$((PASS + 1))
}
say_fail() {
   echo "FAIL  $1"
   FAIL=$((FAIL + 1))
}
say_note() {
   echo "NOTE  $1"
   NOTE=$((NOTE + 1))
}

HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-coexist-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SESSION_ID="coexist$$"
mkdir -p "$AIMEE_HOME/modules.d/server" "$HOME/workflows"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
cp "$GRANT" "$AIMEE_HOME/modules.d/server/db1.grant"
[ -r "$WFE_GRANT" ] && cp "$WFE_GRANT" "$AIMEE_HOME/modules.d/server/wfe.grant"

state() {
   curl -s --unix-socket "$HTTP_SOCK" http://localhost/v1/server/health |
      sed -n 's/.*"state":"\([a-z]*\)".*/\1/p'
}

# Which processes hold the store open, by name. This is the whole question, so
# it is asked of the kernel rather than inferred from what the code says.
holders() {
   for d in /proc/[0-9]*; do
      p=${d#/proc/}
      if ls -l "$d/fd" 2>/dev/null | grep -q "$DB"; then
         printf '%s(%s) ' "$(tr -d '\0' <"$d/comm" 2>/dev/null)" "$p"
      fi
   done
}

cols() {
   sqlite3 "$DB" "select group_concat(name,',') from pragma_table_info('$1')" 2>/dev/null
}
tablecount() {
   sqlite3 "$DB" "select count(*) from sqlite_master where type='table'" 2>/dev/null
}

echo "=============================================================="
echo " 1. daemon + module, as the other scripts test it"
echo "=============================================================="
"$SERVER" --foreground >"$HOME/server.log" 2>&1 &
SPID=$!
i=0
while [ $i -lt 300 ]; do
   [ -S "$HTTP_SOCK" ] && break
   sleep 0.1
   i=$((i + 1))
done
AIMEE_DB1_PATH="$DB" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
MPID=$!
i=0
while [ $i -lt 150 ]; do
   [ "$(state)" = "ok" ] && break
   sleep 0.2
   i=$((i + 1))
done
[ "$(state)" = "ok" ] && say_pass "daemon and module up, store healthy" ||
   say_fail "two-process topology did not come up"

TABLES_BEFORE=$(tablecount)
WI_BEFORE=$(cols lifecycle_work_item)
EV_BEFORE=$(cols lifecycle_event)
echo "      tables: $TABLES_BEFORE"
echo "      holders of aimee.db: $(holders)"
HOLDERS_2=$(holders | wc -w | tr -d ' ')
[ "$HOLDERS_2" = "1" ] &&
   say_pass "exactly one process holds the store (the module)" ||
   say_fail "$HOLDERS_2 processes hold the store before the WFE starts"

echo
echo "=============================================================="
echo " 2. add the Go WFE, the way the entrypoint starts it"
echo "=============================================================="
if [ ! -x "$WFE" ]; then
   say_fail "no aimee-wfe at $WFE -- this is the binary the container runs"
   exit 1
fi
# --home and no --db, exactly as server-entrypoint.sh does it.
AIMEE_MODULE_BUS_SOCKET="$BUS_SOCK" "$WFE" \
   --home "$AIMEE_HOME" \
   --socket "$AIMEE_HOME/aimee-wfe.sock" \
   --workflow-dir "$HOME/workflows" \
   >"$HOME/wfe.log" 2>&1 &
WPID=$!
i=0
while [ $i -lt 100 ]; do
   kill -0 "$WPID" 2>/dev/null || break
   [ -n "$(holders)" ] && [ "$(holders | wc -w | tr -d ' ')" -ge 2 ] && break
   sleep 0.2
   i=$((i + 1))
done

if kill -0 "$WPID" 2>/dev/null; then
   say_pass "the Go WFE started"
else
   say_note "the Go WFE exited early; its output was:"
   tail -15 "$HOME/wfe.log" 2>/dev/null | sed 's/^/      /'
fi

echo "      holders of aimee.db now: $(holders)"
HOLDERS_3=$(holders | wc -w | tr -d ' ')
if [ "$HOLDERS_3" -ge 2 ]; then
   say_fail "$HOLDERS_3 processes hold the store: the module is not its sole owner in the shipped topology"
else
   say_pass "still only one holder of the store"
fi

echo
echo "=============================================================="
echo " 3. does the Go side rewrite the module's schema"
echo "=============================================================="
TABLES_AFTER=$(tablecount)
WI_AFTER=$(cols lifecycle_work_item)
EV_AFTER=$(cols lifecycle_event)
echo "      tables: $TABLES_BEFORE -> $TABLES_AFTER"
[ "$TABLES_AFTER" = "$TABLES_BEFORE" ] &&
   say_pass "no table added or dropped by the Go WFE" ||
   say_note "table count changed: $TABLES_BEFORE -> $TABLES_AFTER"

if [ "$WI_AFTER" = "$WI_BEFORE" ]; then
   say_pass "lifecycle_work_item columns unchanged"
else
   say_fail "the Go WFE altered lifecycle_work_item"
   echo "      before: $WI_BEFORE"
   echo "      after:  $WI_AFTER"
fi
if [ "$EV_AFTER" = "$EV_BEFORE" ]; then
   say_pass "lifecycle_event columns unchanged"
else
   say_fail "the Go WFE altered lifecycle_event"
   echo "      before: $EV_BEFORE"
   echo "      after:  $EV_AFTER"
fi

echo
echo "=============================================================="
echo " 4. concurrent writes from both owners"
echo "=============================================================="
# Drive the module hard through the daemon while the Go side is live on the same
# file, and see whether either reports a lock it could not get.
n=1
ERRORS=0
while [ $n -le 40 ]; do
   r=$(curl -s --unix-socket "$HTTP_SOCK" -X POST -H 'Content-Type: application/json' \
      -d "{\"title\":\"coexist $n\"}" http://localhost/v1/sessions/create)
   case "$r" in
   *'"session_id"'*) ;;
   *) ERRORS=$((ERRORS + 1)) ;;
   esac
   n=$((n + 1))
done
echo "      40 session creates, $ERRORS failed"
[ "$ERRORS" -eq 0 ] && say_pass "every write through the module succeeded" ||
   say_fail "$ERRORS of 40 writes failed with both processes on the store"

LOCKED=$(grep -ic "database is locked\|SQLITE_BUSY\|database table is locked" \
   "$HOME/module.log" "$HOME/wfe.log" "$HOME/server.log" 2>/dev/null |
   awk -F: '{s+=$2} END {print s+0}')
[ "$LOCKED" -eq 0 ] && say_pass "no lock contention reported by any process" ||
   say_fail "$LOCKED lock complaints across the three logs"

echo
echo "      --- WFE log ---"
tail -12 "$HOME/wfe.log" 2>/dev/null | sed 's/^/      /'
echo "      --- module log ---"
tail -8 "$HOME/module.log" 2>/dev/null | sed 's/^/      /'

kill "$WPID" "$MPID" "$SPID" 2>/dev/null
sleep 1

echo
echo "=============================================================="
echo " 5. the other startup order: Go WFE first, module second"
echo "=============================================================="
# Phase 2 had the module create the schema and the Go side amend it. Nothing
# guarantees that order in a container -- both are started by the same
# entrypoint. This runs it the other way: the Go WFE creates the lifecycle
# tables ITS way on an empty file, and the module then has to live with them.
HOME2=$(mktemp -d "${TMPDIR:-/tmp}/aimee-coexist-rev-XXXXXX")
export AIMEE_HOME="$HOME2/.config/aimee"
mkdir -p "$AIMEE_HOME/modules.d/server" "$HOME2/workflows"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
cp "$GRANT" "$AIMEE_HOME/modules.d/server/db1.grant"
[ -r "$WFE_GRANT" ] && cp "$WFE_GRANT" "$AIMEE_HOME/modules.d/server/wfe.grant"

"$WFE" --home "$AIMEE_HOME" --socket "$AIMEE_HOME/aimee-wfe.sock" \
   --workflow-dir "$HOME2/workflows" >"$HOME2/wfe.log" 2>&1 &
WPID2=$!
i=0
while [ $i -lt 60 ]; do
   [ -s "$DB" ] && break
   sleep 0.2
   i=$((i + 1))
done
GO_TABLES=$(tablecount)
GO_WI=$(cols lifecycle_work_item)
echo "      tables created by the Go WFE alone: ${GO_TABLES:-0}"
[ "${GO_TABLES:-0}" -gt 0 ] &&
   say_fail "the Go WFE creates the store when it gets there first (${GO_TABLES} tables)" ||
   say_pass "the Go WFE created nothing on its own"

"$SERVER" --foreground >"$HOME2/server.log" 2>&1 &
SPID=$!
i=0
while [ $i -lt 300 ]; do
   [ -S "$HTTP_SOCK" ] && break
   sleep 0.1
   i=$((i + 1))
done
AIMEE_DB1_PATH="$DB" "$MODULE" "$BUS_SOCK" >"$HOME2/module.log" 2>&1 &
MPID=$!
i=0
while [ $i -lt 150 ]; do
   [ "$(state)" = "ok" ] && break
   sleep 0.2
   i=$((i + 1))
done
if [ "$(state)" = "ok" ]; then
   say_pass "the module came up on a store the Go WFE created first"
else
   say_fail "the module did NOT come up on a Go-created store: $(state)"
   tail -12 "$HOME2/module.log" 2>/dev/null | sed 's/^/      /'
fi

REV_TABLES=$(tablecount)
REV_WI=$(cols lifecycle_work_item)
echo "      tables after the module joined: ${REV_TABLES:-0}"
if [ "$REV_WI" = "$WI_AFTER" ]; then
   say_pass "lifecycle_work_item ends up the same shape either way"
else
   say_note "lifecycle_work_item shape depends on startup order"
   echo "      module-first: $WI_AFTER"
   echo "      wfe-first:    $REV_WI"
fi

R=$(curl -s --unix-socket "$HTTP_SOCK" -X POST -H "Content-Type: application/json" \
   -d "{\"title\":\"reverse order\"}" http://localhost/v1/sessions/create)
case "$R" in
*"session_id"*) say_pass "the store works when the Go WFE built it first" ;;
*) say_fail "store unusable in this order: $R" ;;
esac
kill "$WPID2" "$MPID" "$SPID" 2>/dev/null

echo
echo "=============================================================="
echo " $PASS passed, $FAIL failed, $NOTE noted"
echo " home: $HOME"
echo "=============================================================="
kill "$WPID" "$MPID" "$SPID" 2>/dev/null
[ "$FAIL" -eq 0 ]
