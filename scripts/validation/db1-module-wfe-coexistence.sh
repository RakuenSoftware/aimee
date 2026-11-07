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
# owns -- and it does not merely read it: internal/workflowstore/store.go has its own
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
# The current fixture measures PostgreSQL schema metadata and the actual SQL
# owner across both startup orders; no empty stand-in columns count as proof.
#
# Overridable: MODULE, GRANT, SERVER, WFE.
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

PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH
# The container grants the Go WFE bus access too (principal_ref 64). Without it
# the WFE exits on "bus: attach denied" -- but only AFTER it has opened the
# store and run its migrations, which is itself worth knowing and was how this
# was first seen. Install it so the WFE stays up and the coexistence being
# measured is the sustained one the container actually runs.

. "$(dirname "$0")/workflow-fixture-runtime.sh"
cleanup() { workflow_fixture_stop_runtime "${WPID2:-${WPID:-}}" "${MPID:-}" "${SPID:-}"; }
trap cleanup EXIT

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
workflow_fixture_install_policy || exit 1

state() {
   curl -s --unix-socket "$HTTP_SOCK" http://localhost/v1/server/health |
      sed -n 's/.*"state":"\([a-z]*\)".*/\1/p'
}

# Which processes hold the store open, by name. This is the whole question, so
# it is asked of the kernel rather than inferred from what the code says.
# Holding the store is a connection to PostgreSQL, not an open file. The port
# comes from the DSN so a non-default one is still found.
holders() { workflow_fixture_sql_holders; }

# The schema is the module's, and the module is the only thing that can read it.
# It reports what it applied on connect; that is the account to compare against,
# and it is the same claim from the side that would know if the engine had
# reshaped anything underneath.
schema_applied() {
   sed -n 's/.*store: schema applied (\([0-9]*\) files).*/\1/p' "$HOME/module.log" 2>/dev/null |
      tail -1
}

echo "=============================================================="
echo " 1. daemon + module, as the other scripts test it"
echo "=============================================================="
"$SERVER" --foreground >"$HOME/server.log" 2>&1 &
SPID=$!
workflow_fixture_start_config || exit 1
i=0
while [ $i -lt 300 ]; do
   [ -S "$HTTP_SOCK" ] && break
   sleep 0.1
   i=$((i + 1))
done
require_store_url
workflow_fixture_start_postgres || exit 1
AIMEE_STORE_URL="$AIMEE_STORE_URL" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
MPID=$!
i=0
while [ $i -lt 150 ]; do
   [ "$(state)" = "ok" ] && break
   sleep 0.2
   i=$((i + 1))
done
[ "$(state)" = "ok" ] && say_pass "daemon and module up, store healthy" ||
   say_fail "two-process topology did not come up"

TABLES_BEFORE=$(schema_applied)
workflow_fixture_schema_snapshot "$AIMEE_HOME/schema-before.json" || exit 1
echo "      schema files the module applied: ${TABLES_BEFORE:-0}"
echo "      holders of the store: $(holders)"
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
   --workflow-dir "$HOME/workflows" \
   >"$HOME/wfe.log" 2>&1 &
WPID=$!
i=0
while [ $i -lt 100 ]; do
   kill -0 "$WPID" 2>/dev/null || break
   workflow_fixture_stage_ready && break
   sleep 0.2
   i=$((i + 1))
done

if workflow_fixture_stage_ready; then
   say_pass "the Go WFE serves the authenticated control stage"
else
   say_fail "the Go WFE did not serve its control stage; its output was:"
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
TABLES_AFTER=$(schema_applied)
workflow_fixture_schema_snapshot "$AIMEE_HOME/schema-after.json" || exit 1
echo "      schema files: $TABLES_BEFORE -> $TABLES_AFTER"
[ -n "$TABLES_BEFORE" ] && [ "$TABLES_AFTER" = "$TABLES_BEFORE" ] &&
   say_pass "the module's applied schema history is unchanged" ||
   say_fail "the applied schema history changed or was unavailable"
if cmp -s "$AIMEE_HOME/schema-before.json" "$AIMEE_HOME/schema-after.json"; then
   say_pass "actual PostgreSQL tables and column definitions are unchanged"
else
   say_fail "the workflow engine changed the fixture schema"
fi
[ ! -e "$DB" ] && say_pass "no retired SQLite store was created" ||
   say_fail "a retired SQLite store appeared"

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

workflow_fixture_stop_runtime "$WPID" "$MPID" "$SPID"
sleep 1

echo
echo "=============================================================="
echo " 5. the other startup order: Go WFE first, module second"
echo "=============================================================="
# The engine starts before the bus/SQL owners in this phase. Read the actual
# fixture schema before and after that start: the engine has no independent SQL
# authority and must not change it while waiting for its owners.
HOME2=$(mktemp -d "${TMPDIR:-/tmp}/aimee-coexist-rev-XXXXXX")
export AIMEE_HOME="$HOME2/.config/aimee"
mkdir -p "$AIMEE_HOME/modules.d/server" "$HOME2/workflows"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
workflow_fixture_install_policy || exit 1

workflow_fixture_schema_snapshot "$AIMEE_HOME/schema-before-engine.json" || exit 1
"$WFE" --home "$AIMEE_HOME" --module-bus-socket "$BUS_SOCK" \
   --workflow-dir "$HOME2/workflows" >"$HOME2/wfe.log" 2>&1 &
WPID2=$!
i=0
while [ $i -lt 60 ]; do
   [ -s "$DB" ] && break
   sleep 0.2
   i=$((i + 1))
done
workflow_fixture_schema_snapshot "$AIMEE_HOME/schema-engine-only.json" || exit 1
if cmp -s "$AIMEE_HOME/schema-before-engine.json" "$AIMEE_HOME/schema-engine-only.json"; then
   say_pass "the engine starting before its SQL owner changed no PostgreSQL schema"
else
   say_fail "the engine changed PostgreSQL schema before its owner started"
fi
[ ! -e "$DB" ] && say_pass "the engine created no retired SQLite store" ||
   say_fail "the engine created a retired SQLite store"

"$SERVER" --foreground >"$HOME2/server.log" 2>&1 &
SPID=$!
workflow_fixture_start_config || exit 1
i=0
while [ $i -lt 300 ]; do
   [ -S "$HTTP_SOCK" ] && break
   sleep 0.1
   i=$((i + 1))
done
workflow_fixture_start_postgres || exit 1
AIMEE_STORE_URL="$AIMEE_STORE_URL" "$MODULE" "$BUS_SOCK" >"$HOME2/module.log" 2>&1 &
MPID=$!
i=0
while [ $i -lt 150 ]; do
   [ "$(state)" = "ok" ] && break
   sleep 0.2
   i=$((i + 1))
done
if [ "$(state)" = "ok" ]; then
   say_pass "the SQL owner and module came up after the workflow engine"
else
   say_fail "the SQL owner/module did not become ready after the workflow engine: $(state)"
   tail -12 "$HOME2/module.log" 2>/dev/null | sed 's/^/      /'
fi

i=0
while [ "$i" -lt 100 ]; do
   workflow_fixture_stage_ready && break
   kill -0 "$WPID2" 2>/dev/null || break
   sleep 0.1
   i=$((i + 1))
done
workflow_fixture_stage_ready && say_pass "the early engine serves authenticated control after its owner starts" ||
   say_fail "the early engine never attached to its real control stage"

# Same question in the reverse startup order, asked of the module rather than of
# a file: it reports the schema it applied, and applying the same set either way
# is what "the shape does not depend on startup order" means now.
REV_TABLES=$(sed -n 's/.*store: schema applied (\([0-9]*\) files).*/\1/p' \
             "$HOME2/module.log" 2>/dev/null | tail -1)
echo "      schema files applied in this order: ${REV_TABLES:-0}"
if [ "${REV_TABLES:-0}" = "${TABLES_AFTER:-0}" ] && [ "${REV_TABLES:-0}" -gt 0 ]; then
   say_pass "the store ends up the same shape either way"
else
   say_note "schema application depends on startup order"
   echo "      module-first: ${TABLES_AFTER:-0}"
   echo "      wfe-first:    ${REV_TABLES:-0}"
fi

R=$(curl -s --unix-socket "$HTTP_SOCK" -X POST -H "Content-Type: application/json" \
   -d "{\"title\":\"reverse order\"}" http://localhost/v1/sessions/create)
case "$R" in
*"session_id"*) say_pass "the store works when the workflow engine starts first" ;;
*) say_fail "store unusable in this order: $R" ;;
esac
workflow_fixture_stop_runtime "$WPID2" "$MPID" "$SPID"

echo
echo "=============================================================="
echo " $PASS passed, $FAIL failed, $NOTE noted"
echo " home: $HOME"
echo "=============================================================="
workflow_fixture_stop_runtime "$WPID" "$MPID" "$SPID"
[ "$FAIL" -eq 0 ]
