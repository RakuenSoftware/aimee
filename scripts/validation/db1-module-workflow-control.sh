#!/bin/sh
# The workflow control plane on a deployed shape.
#
# The sibling scripts here all check the STORE: who owns aimee.db, what survives
# a module restart, what happens under contention. None of them touch the
# workflow surface, and that gap had a cost -- every /v1/workflow route and
# /v1/dev/submit answered 503 for anyone who started the engine with its
# documented --module-bus-socket flag, because the control stage read the
# environment variable instead. Six green validation scripts and a green
# integration suite said nothing, because nothing asked.
#
# So this asks. It starts the three processes a deploy starts -- daemon, DB1
# module, Go engine -- installs the three grants a deploy installs, and gives
# the engine its bus socket BY FLAG with AIMEE_MODULE_BUS_SOCKET explicitly
# unset, which is precisely the case that used to fail. Then it drives the
# routes a client actually calls.
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

# pct exec hands over a minimal PATH that does not include /usr/local/bin,
# which is where the deploy puts the binaries.
PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH

# Overridable so this runs against a build tree as well as an install.
MODULE=${AIMEE_DB1_MODULE:-/usr/local/libexec/aimee-modules/aimee-module-aimee}
WFE=${AIMEE_WFE_BIN:-/usr/local/bin/aimee-wfe}
GRANT_DIR=${AIMEE_GRANT_DIR:-/opt/payload/grants}
WORKFLOW_SRC=${AIMEE_WORKFLOW_DIR:-/opt/workflows}

for _need in "$MODULE" "$WFE"; do
   if [ ! -x "$_need" ]; then
      echo "ABORT: $_need is not executable here; this script needs a deployed shape."
      exit 1
   fi
done

HOME=$(mktemp -d "${TMPDIR:-/tmp}/aimee-wfctl-XXXXXX")
export HOME
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SESSION_ID="wfctl$$"
mkdir -p "$AIMEE_HOME/modules.d/server" "$AIMEE_HOME/workflows"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"

# The definitions a deploy ships. Without them the engine starts on an empty
# registry and every submit fails to resolve its workflow, which reads like a
# broken intake and is not one.
cp "$WORKFLOW_SRC"/*.yaml "$AIMEE_HOME/workflows/" 2>/dev/null

# All three grants. The engine needs TWO for the same executable: a serving
# identity for the control kinds it answers (workflows, ref 20) and a separate
# outbound identity for the store kinds it calls (wfe, ref 64). A module's
# serving grant requests nothing, so one grant cannot do both.
for g in db1 wfe workflows; do
   if [ ! -r "$GRANT_DIR/$g.grant" ]; then
      echo "ABORT: no $g grant at $GRANT_DIR/$g.grant."
      exit 1
   fi
   # Each grant must name the binary THIS tree runs, not the installed
   # path baked into the generated bundle.
   case "$g" in
   db1) _exe="$MODULE" ;;
   wfe | workflows) _exe="${WFE:-${AIMEE_WFE_BIN:-}}" ;;
   *) _exe="" ;;
   esac
   if [ -n "$_exe" ]; then
      sed "s|^executable=.*|executable=$_exe|" "$GRANT_DIR/$g.grant" \
         >"$AIMEE_HOME/modules.d/server/$g.grant"
   else
      install -m0644 "$GRANT_DIR/$g.grant" \
         "$AIMEE_HOME/modules.d/server/$g.grant"
   fi
done

SERVER_PID=""
MODULE_PID=""
WFE_PID=""
cleanup() {
   for p in $WFE_PID $MODULE_PID $SERVER_PID; do
      kill "$p" 2>/dev/null
   done
}
trap cleanup EXIT

# verb path [body] -> prints the status code; body lands in $BODY_FILE.
BODY_FILE="$HOME/response.body"
api() {
   if [ -n "$3" ]; then
      curl -s -o "$BODY_FILE" -w '%{http_code}' --unix-socket "$HTTP_SOCK" -X "$1" \
         -H 'Content-Type: application/json' -d "$3" "http://localhost$2"
   else
      curl -s -o "$BODY_FILE" -w '%{http_code}' --unix-socket "$HTTP_SOCK" -X "$1" \
         "http://localhost$2"
   fi
}
body() { cat "$BODY_FILE" 2>/dev/null; }

aimee-server --foreground >"$HOME/daemon.log" 2>&1 &
SERVER_PID=$!
i=0
while [ $i -lt 300 ]; do
   [ -S "$HTTP_SOCK" ] && break
   kill -0 "$SERVER_PID" 2>/dev/null || break
   sleep 0.1
   i=$((i + 1))
done
require_store_url
AIMEE_STORE_URL="$AIMEE_STORE_URL" "$MODULE" "$BUS_SOCK" >"$HOME/module.log" 2>&1 &
MODULE_PID=$!
sleep 1

# THE POINT OF THIS SCRIPT: the flag alone, with the variable cleared.
unset AIMEE_MODULE_BUS_SOCKET
"$WFE" --home "$AIMEE_HOME" --socket "$AIMEE_HOME/aimee-wfe.sock" \
   --module-bus-socket "$BUS_SOCK" >"$HOME/wfe.log" 2>&1 &
WFE_PID=$!

# Attachment is what matters, not the process: poll the seam itself until it
# stops answering "not attached". A fixed sleep would be a stall or a flake.
ATTACHED=no
i=0
while [ $i -lt 150 ]; do
   if [ "$(api GET /v1/workflow/defs)" = "200" ]; then
      ATTACHED=yes
      break
   fi
   kill -0 "$WFE_PID" 2>/dev/null || break
   sleep 0.2
   i=$((i + 1))
done

echo "=============================================================="
echo " 1. the control stage is served when the socket comes by FLAG"
echo "=============================================================="
ck_eq "the control stage is attached (this answered 503 before)" yes "$ATTACHED"
ck_eq "GET /v1/workflow/defs answers 200" 200 "$(api GET /v1/workflow/defs)"
ck_has "and serves the shipped definitions" "$(body)" "build"
ck_eq "GET /v1/workflow/items answers" 200 "$(api GET /v1/workflow/items)"
ck_eq "GET /v1/workflow/triggers answers" 200 "$(api GET /v1/workflow/triggers)"

echo
echo "=============================================================="
echo " 2. intake refuses before it records"
echo "=============================================================="
ck_eq "a submit with no proposal is refused" 400 "$(api POST /v1/dev/submit '{"repo":"ct/repo"}')"
ck_eq "a submit with no repo is refused" 400 \
   "$(api POST /v1/dev/submit '{"proposal_md":"# no repo"}')"

echo
echo "=============================================================="
echo " 3. a run is admitted, and owns what it was given"
echo "=============================================================="
CODE=$(api POST /v1/dev/submit \
   '{"proposal_md":"# ct one\n\nthe first thing","workflow":"build","repo":"ct/repo"}')
B1=$(body)
ck_eq "a submit is admitted" 200 "$CODE"
ck_has "and names the run it started" "$B1" "work_item_id"
WI1=$(printf '%s' "$B1" | sed -n 's/.*"work_item_id"[^"]*"\([^"]*\)".*/\1/p')
CODE=$(api POST /v1/dev/submit \
   '{"proposal_md":"# ct two\n\nthe second thing","workflow":"build","repo":"ct/repo"}')
WI2=$(body | sed -n 's/.*"work_item_id"[^"]*"\([^"]*\)".*/\1/p')
ck_eq "a second submit is admitted" 200 "$CODE"
ck "the two runs are distinct" test -n "$WI2" -a "$WI1" != "$WI2"

# Two runs must not share one proposal artifact. If they did, the later submit
# silently replaced the earlier one's instructions and the first run would go on
# to execute work nobody asked for -- a wrong answer, not an error.
ck "the first run stored its own proposal" test -s "$AIMEE_HOME/wfe-artifacts/$WI1/proposal.md"
ck "the second stored a separate one" test -s "$AIMEE_HOME/wfe-artifacts/$WI2/proposal.md"
ck_has "and the first still says what it said" \
   "$(cat "$AIMEE_HOME/wfe-artifacts/$WI1/proposal.md" 2>/dev/null)" "the first thing"
ck_has "while the second says its own thing" \
   "$(cat "$AIMEE_HOME/wfe-artifacts/$WI2/proposal.md" 2>/dev/null)" "the second thing"

echo
echo "=============================================================="
echo " 4. admission refuses rather than admitting forever"
echo "=============================================================="
# Submit until something refuses rather than assuming which attempt crosses the
# line: the cap is a policy value, and hard-coding "the third one" fails for the
# wrong reason the day the default moves.
CAP_CODE=""
CAP_BODY=""
i=1
while [ $i -le 8 ]; do
   CAP_CODE=$(api POST /v1/dev/submit \
      "{\"proposal_md\":\"# cap probe $i\",\"workflow\":\"build\",\"repo\":\"ct/repo\"}")
   CAP_BODY=$(body)
   [ "$CAP_CODE" != "200" ] && break
   i=$((i + 1))
done
ck_eq "admission caps, and says so with 409" 409 "$CAP_CODE"
ck_has "naming the cap rather than a broken store" "$CAP_BODY" "admission full"

echo
echo "=============================================================="
echo " 5. the run reads back through the bus"
echo "=============================================================="
CODE=$(api GET "/v1/workflow/items/$WI1")
ITEM=$(body)
ck_eq "the run is fetchable by id" 200 "$CODE"
ck_has "and reports its stage" "$ITEM" '"stage"'
ck_has "and its submitter" "$ITEM" '"submitter"'
ck_eq "its events are served" 200 "$(api GET "/v1/workflow/items/$WI1/events")"
ck_eq "its proposal is served" 200 "$(api GET "/v1/workflow/items/$WI1/proposal")"
ck_eq "an unknown run is 404" 404 "$(api GET /v1/workflow/items/wi_no_such_run)"

echo
echo "=============================================================="
echo " 6. with no runner configured a run parks, and says why"
echo "=============================================================="
# The behaviour worth pinning: admitted, driven, and PARKED naming what it
# lacked. An intake that admitted a run and then left it silently "active"
# would look identical from outside on the day the runner really was broken.
PARKED=0
i=0
while [ $i -lt 60 ]; do
   case "$(api GET "/v1/workflow/items/$WI1" >/dev/null; body)" in
   *runner_unavailable*)
      PARKED=1
      break
      ;;
   esac
   sleep 0.5
   i=$((i + 1))
done
ck_eq "the run parks instead of stalling silently" 1 "$PARKED"
ck_eq "resuming a park the operator cannot clear is refused" 409 \
   "$(api POST "/v1/workflow/items/$WI1/resume" '{}')"

echo
echo "=============================================================="
echo " 7. operator controls reach the store"
echo "=============================================================="
ck_eq "the run can be stopped" 200 "$(api POST "/v1/workflow/items/$WI1/stop" '{}')"
ck_eq "stopping frees the admission slot it held" 200 \
   "$(api POST /v1/dev/submit \
      '{"proposal_md":"# after a slot freed","workflow":"build","repo":"ct/repo"}')"
ck_eq "a run can be deleted" 200 "$(api DELETE "/v1/workflow/items/$WI2")"
ck_eq "and it is gone from the read side" 404 "$(api GET "/v1/workflow/items/$WI2")"

echo
echo "=============================================================="
echo " 8. and the daemon still holds no database"
echo "=============================================================="
# The migration's standing claim, re-asserted on the shape that drives the most
# store traffic: everything above went through the module, not through a file
# the daemon opened.
# Holding the store is a connection now, not an open file -- it is PostgreSQL
# behind the module. The daemon's half is unchanged: it must have no database
# descriptor of any kind.
DAEMON_FDS=$(ls -l "/proc/$SERVER_PID/fd" 2>/dev/null | grep -c 'aimee\.db')
ck_eq "the daemon holds no aimee.db descriptor" 0 "$DAEMON_FDS"
# grep -c prints 0 AND exits 1 when nothing matches, so take the count and
# swallow the status rather than appending a second number with || echo.
MODULE_CONNS=$(ss -tnp 2>/dev/null | grep -c "pid=$MODULE_PID,") || true
ck "the module is the one holding it" test "${MODULE_CONNS:-0}" -ge 1

echo
echo "      --- engine log, anything that looks wrong ---"
grep -iE "error|refused|denied|panic" "$HOME/wfe.log" 2>/dev/null | sed 's/^/      /' | head -10

echo
echo "=============================================================="
echo " results: $PASS passed, $FAIL failed"
echo " home: $HOME"
echo "=============================================================="
[ "$FAIL" -eq 0 ]
