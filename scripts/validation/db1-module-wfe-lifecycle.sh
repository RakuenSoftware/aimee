#!/bin/sh
# A run driven through the DEPLOYED workflow engine, end to end.
#
# The other scripts here prove things about the store: who holds it, what it
# creates, whether it survives an upgrade. db1-module-wfe-coexistence.sh even
# starts the engine -- but only to see whether it opens the file. None of them
# drives the engine's own API, so none of them proves the thing the port was
# for: that a run submitted to the workflow engine is created, listed, paused,
# resumed and stopped THROUGH the module, in the topology the container runs.
#
# So this one submits a real run over the engine's socket and, at every step,
# checks the answer against the module's store read independently with sqlite3.
# The API agreeing with itself is not evidence; the API agreeing with the file
# the module owns is.
#
# Two invariants hold throughout, and are re-checked after every mutation:
#
#   the engine holds no descriptor on aimee.db
#   the module is the only process that does
#
# Overridable: MODULE, GRANT, WFE_GRANT, SERVER, WFE.
PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-db1}
GRANT=${AIMEE_DB1_GRANT:-/opt/payload/grants/db1.grant}
WFE_GRANT=${AIMEE_WFE_GRANT:-/opt/payload/grants/wfe.grant}
SERVER=${AIMEE_SERVER_BIN:-/usr/local/bin/aimee-server}
WFE=${AIMEE_WFE_BIN:-/usr/local/bin/aimee-wfe}

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

HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-wfe-life-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
mkdir -p "$AIMEE_HOME/modules.d/server" "$HOME/workflows" "$HOME/repo"
# The engine needs the workflow definitions it would ship with; a submit names
# one by workflow name and fails without it.
WORKFLOWS=${AIMEE_WORKFLOWS_DIR:-/opt/workflows}
[ -d "$WORKFLOWS" ] && cp "$WORKFLOWS"/*.yaml "$HOME/workflows/" 2>/dev/null || true
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
WFE_SOCK="$AIMEE_HOME/aimee-wfe.sock"
DB="$AIMEE_HOME/aimee.db"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
cp "$GRANT" "$AIMEE_HOME/modules.d/server/db1.grant"
cp "$WFE_GRANT" "$AIMEE_HOME/modules.d/server/wfe.grant"

# A git repo for the proposal to belong to: submit resolves the repo path and
# validates the proposal source against it.
(cd "$HOME/repo" && git init -q . && git config user.email t@t && git config user.name t &&
   mkdir -p docs/proposals/pending && printf '# A proposal\n\nDo the thing.\n' \
      >docs/proposals/pending/thing.md && git add -A && git commit -qm init) >/dev/null 2>&1

state() {
   curl -s --unix-socket "$HTTP_SOCK" http://localhost/v1/server/health |
      sed -n 's/.*"state":"\([a-z]*\)".*/\1/p'
}
# Every call carries the same identity. The engine scopes a run to the principal
# that submitted it, and resolves that owner by walking parent links THROUGH the
# module -- so calling as one user and reading as another is not a test of the
# store, it is a test of the access check refusing. Sending the header exercises
# the ownership lookup as well as the operation.
WEBUSER=${WEBUSER:-lifecycle-tester}
wfe() {
   # $1 method, $2 path, $3 body
   if [ -n "$3" ]; then
      curl -s --max-time 20 --unix-socket "$WFE_SOCK" -X "$1" \
         -H "X-Aimee-Webuser: $WEBUSER" \
         -H 'Content-Type: application/json' -d "$3" "http://localhost$2"
   else
      curl -s --max-time 20 --unix-socket "$WFE_SOCK" -X "$1" \
         -H "X-Aimee-Webuser: $WEBUSER" "http://localhost$2"
   fi
}
holders() {
   for d in /proc/[0-9]*; do
      p=${d#/proc/}
      if ls -l "$d/fd" 2>/dev/null | grep -q "$DB"; then
         printf '%s(%s) ' "$(tr -d '\0' <"$d/comm" 2>/dev/null)" "$p"
      fi
   done
}
in_store() {
   sqlite3 "$DB" "$1" 2>/dev/null
}

echo "=============================================================="
echo " the shipped topology: daemon, module, engine"
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
[ "$(state)" = "ok" ] && say_pass "daemon and module up" || {
   say_fail "store never came up"
   exit 1
}

AIMEE_MODULE_BUS_SOCKET="$BUS_SOCK" "$WFE" \
   --home "$AIMEE_HOME" --socket "$WFE_SOCK" --workflow-dir "$HOME/workflows" \
   >"$HOME/wfe.log" 2>&1 &
WPID=$!
i=0
while [ $i -lt 200 ]; do
   [ -S "$WFE_SOCK" ] && break
   kill -0 "$WPID" 2>/dev/null || break
   sleep 0.1
   i=$((i + 1))
done
if [ -S "$WFE_SOCK" ]; then
   say_pass "the workflow engine bound its socket"
else
   say_fail "the engine did not start; its output was:"
   tail -15 "$HOME/wfe.log" 2>/dev/null | sed 's/^/      /'
   kill "$MPID" "$SPID" 2>/dev/null
   exit 1
fi

# The invariant, asserted here and after every mutation below.
check_owner() {
   who=$(holders)
   count=$(printf '%s' "$who" | wc -w | tr -d ' ')
   case "$who" in
   *aimee-wfe*)
      say_fail "$1: the engine holds the store open ($who)"
      return
      ;;
   esac
   if [ "$count" = "1" ]; then
      say_pass "$1: the module is still the only holder"
   else
      say_fail "$1: $count processes hold the store ($who)"
   fi
}
check_owner "at rest"

echo
echo "=============================================================="
echo " a run, submitted to the engine and checked in the store"
echo "=============================================================="
SUBMIT=$(wfe POST /v1/dev/submit \
   "{\"proposal_md\":\"# A proposal\\n\\nDo the thing.\\n\",\"repo\":\"$HOME/repo\",\"workflow\":\"build\",\"source_path\":\"docs/proposals/pending/thing.md\"}")
echo "      submit -> $(printf '%s' "$SUBMIT" | head -c 200)"
WI=$(printf '%s' "$SUBMIT" | sed -n 's/.*"work_item_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
[ -z "$WI" ] && WI=$(printf '%s' "$SUBMIT" | sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
if [ -n "$WI" ]; then
   say_pass "the engine accepted the run ($WI)"
else
   say_fail "submit produced no work item id"
fi

if [ -n "$WI" ]; then
   # The claim that matters: it is in the MODULE's file, not merely in an API
   # response the engine composed for itself.
   rows=$(in_store "SELECT COUNT(*) FROM lifecycle_work_item WHERE work_item_id='$WI'")
   [ "${rows:-0}" = "1" ] && say_pass "and the row is in the module's store" ||
      say_fail "the run is not in the module's store (rows=${rows:-0})"
   # And its creation was recorded, which only happens inside the operation.
   ev=$(in_store "SELECT COUNT(*) FROM lifecycle_event WHERE work_item_id='$WI' AND kind='create'")
   [ "${ev:-0}" -ge 1 ] && say_pass "with the create event the operation writes" ||
      say_fail "no create event was recorded"
   check_owner "after a write"

   LIST=$(wfe GET /v1/workflow/items)
   case "$LIST" in
   *"$WI"*) say_pass "the engine lists it back through the module" ;;
   *) say_fail "the run did not come back from the engine's list: $(printf '%s' "$LIST" | head -c 160)" ;;
   esac

   GET=$(wfe GET "/v1/workflow/items/$WI")
   case "$GET" in
   *"$WI"*) say_pass "and fetches it by id" ;;
   *) say_fail "fetch by id failed: $(printf '%s' "$GET" | head -c 160)" ;;
   esac

   EVENTS=$(wfe GET "/v1/workflow/items/$WI/events")
   case "$EVENTS" in
   *create*) say_pass "and reads its history" ;;
   *) say_fail "events did not include the create: $(printf '%s' "$EVENTS" | head -c 160)" ;;
   esac
fi

echo
echo "=============================================================="
echo " mutations: pause, resume, stop -- each checked in the store"
echo "=============================================================="
if [ -n "$WI" ]; then
   # With no runner configured the scheduler parks this run on its own, and it
   # may do so between any two statements here -- so the expectation is derived
   # from the OUTCOME rather than from a pre-read that the scheduler can
   # invalidate.
   #
   # A park is guarded on pause_reason='' in the module. So there are exactly
   # two consistent worlds, and both are asserted:
   #
   #   accepted  the run was runnable, and the store now carries a reason.
   #   refused   the run was already parked, and the store proves it by carrying
   #             a reason that is not the one this call would have written.
   #
   # What must never happen is a refusal with an unparked run, or an acceptance
   # that left no reason behind: either would mean the guard and the write
   # disagree.
   PAUSE=$(wfe POST "/v1/workflow/items/$WI/pause" '{}')
   reason=$(in_store "SELECT pause_reason FROM lifecycle_work_item WHERE work_item_id='$WI'")
   case "$PAUSE" in
   *'"ok":true'*)
      [ -n "$reason" ] &&
         say_pass "the pause was accepted and the store carries the reason ($reason)" ||
         say_fail "the pause was accepted but the store has no reason"
      ;;
   *)
      # A third consistent world, missed by the two above. The module guards the
      # park on FOUR columns:
      #
      #   work_item_id=? AND current_stage=? AND state='active' AND pause_reason=''
      #
      # so a refusal also happens when the run has left the stage this call named,
      # or is no longer active, or is being parked by the scheduler right now --
      # and in that instant pause_reason can legitimately still read empty,
      # because the read is a separate statement from the operation (as the
      # comment above says). Asserting "refused => a reason is already there"
      # therefore failed intermittently on a run the scheduler happened to be
      # touching, which is the engine working, not the guard disagreeing.
      #
      # Re-read once after a settle. The genuine defect -- the guard refusing a
      # park that the store says was perfectly parkable -- is a run that is STILL
      # active, still on the same stage, and still carries no reason. That is
      # what is asserted now.
      if [ -z "$reason" ]; then
         sleep 2
         reason=$(in_store "SELECT pause_reason FROM lifecycle_work_item WHERE work_item_id='$WI'")
         state_now=$(in_store "SELECT state FROM lifecycle_work_item WHERE work_item_id='$WI'")
      fi
      if [ -n "$reason" ]; then
         say_pass "the pause was refused because the run was already parked ($reason), and that reason stands"
      elif [ "${state_now:-active}" != "active" ]; then
         say_pass "the pause was refused because the run was no longer active (state=$state_now)"
      else
         say_fail "the pause was refused but the run is still active, on the same stage, and unparked: $(printf '%s' "$PAUSE" | head -c 160)"
      fi
      ;;
   esac
   check_owner "after a pause"

   # Resume goes through the operator allowlist in the module. A run parked by
   # the scheduler for want of a runner is NOT operator-resumable, and refusing
   # that is the behaviour the allowlist exists for -- so what is asserted is
   # that the store and the engine agree about the outcome, either way.
   # Re-read immediately before the resume: `reason` above was read at pause time,
   # and a scheduler park landing in between made this compare a stale value
   # against a fresh one and report a "cleared" pause that had in fact just been
   # SET. What this asserts is that the resume did not clear a lifecycle-owned
   # reason, so the reason it must compare is the one in force when it resumed.
   reason=$(in_store "SELECT pause_reason FROM lifecycle_work_item WHERE work_item_id='$WI'")
   wfe POST "/v1/workflow/items/$WI/resume" '{}' >/dev/null
   after=$(in_store "SELECT pause_reason FROM lifecycle_work_item WHERE work_item_id='$WI'")
   if [ "$reason" = "manual" ]; then
      [ -z "$after" ] && say_pass "an operator pause resumed and the store agrees" ||
         say_fail "an operator pause did not clear (reason=$after)"
   else
      [ "$after" = "$reason" ] &&
         say_pass "a lifecycle-owned pause ($reason) was not cleared by a generic resume" ||
         say_fail "a lifecycle-owned pause was cleared: $reason -> $after"
   fi

   wfe POST "/v1/workflow/items/$WI/stop" '{}' >/dev/null
   st=$(in_store "SELECT state FROM lifecycle_work_item WHERE work_item_id='$WI'")
   [ "$st" = "stopped" ] && say_pass "stop reached the store (state=stopped)" ||
      say_fail "stop did not reach the store (state=$st)"
   check_owner "after a stop"

   # A stopped run's terminal event is written by the same transaction that
   # stopped it, so its absence would mean the tree operation half-applied.
   term=$(in_store "SELECT COUNT(*) FROM lifecycle_event WHERE work_item_id='$WI' AND kind='terminal'")
   [ "${term:-0}" -ge 1 ] && say_pass "with the terminal event from the same transaction" ||
      say_fail "the stop wrote no terminal event"
fi

echo
echo "=============================================================="
echo " the module restarts under the engine"
echo "=============================================================="
# The engine's store is another process now, so that process dying is an
# ordinary event: a module upgrade, a crash, a supervisor restart. What must not
# happen is the engine wedging, corrupting a run, or reporting success while the
# store is gone.
#
# The engine attaches to the DAEMON's bus, not to the module, so its attachment
# survives; the module re-attaches and serves again. Whether a call issued after
# that recovers is the thing worth knowing, and it is not something reading the
# code answers -- there is no reconnect logic in the Go client, and it may not
# need any.
kill "$MPID" 2>/dev/null
wait "$MPID" 2>/dev/null
MPID=""
DOWN=$(wfe GET /v1/workflow/items)
case "$DOWN" in
*'"items":['*'wi_'*)
   say_fail "the engine served work items from somewhere with the store gone: $(printf '%s' "$DOWN" | head -c 120)"
   ;;
*)
   say_pass "with the module gone the engine does not invent an answer"
   ;;
esac
if kill -0 "$WPID" 2>/dev/null; then
   say_pass "and the engine is still alive rather than crashed"
else
   say_fail "the engine died when its store went away"
fi

AIMEE_DB1_PATH="$DB" "$MODULE" "$BUS_SOCK" >>"$HOME/module.log" 2>&1 &
MPID=$!
i=0
while [ $i -lt 150 ]; do
   [ "$(state)" = "ok" ] && break
   sleep 0.2
   i=$((i + 1))
done
[ "$(state)" = "ok" ] && say_pass "the module came back" || say_fail "the module did not come back"

# The engine has to reach the NEW module process through the attachment it
# already held. If this fails, the engine needs reconnect logic it does not
# currently have -- which is exactly what this is here to find out.
BACK=""
i=0
while [ $i -lt 40 ]; do
   BACK=$(wfe GET /v1/workflow/items)
   case "$BACK" in
   *"$WI"*) break ;;
   esac
   sleep 0.25
   i=$((i + 1))
done
case "$BACK" in
*"$WI"*) say_pass "and the engine reaches it again without being restarted" ;;
*) say_fail "the engine could not reach the restarted module: $(printf '%s' "$BACK" | head -c 160)" ;;
esac

# And the run is intact, not half-written by whatever was in flight.
st=$(in_store "SELECT state FROM lifecycle_work_item WHERE work_item_id='$WI'")
[ "$st" = "stopped" ] && say_pass "the run survived the restart unchanged (state=$st)" ||
   say_fail "the run changed across the restart (state=$st)"
check_owner "after the restart"

echo
echo "      --- engine log, anything that looks wrong ---"
grep -iE "error|refused|denied|cannot" "$HOME/wfe.log" 2>/dev/null |
   grep -viE "roundtable|review|forge|runner" | head -8 | sed 's/^/      /'

echo
echo "=============================================================="
echo " $PASS passed, $FAIL failed"
echo " home: $HOME"
echo "=============================================================="
kill "$WPID" "$MPID" "$SPID" 2>/dev/null
[ "$FAIL" -eq 0 ]
