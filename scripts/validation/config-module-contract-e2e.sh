#!/bin/sh
# Real-process config contract test: daemon bus host + DB1 + config + Go WFE.
set -eu

SERVER=${AIMEE_SERVER_BIN:?set AIMEE_SERVER_BIN}
DB1=${AIMEE_DB1_MODULE:?set AIMEE_DB1_MODULE}
CONFIG=${AIMEE_CONFIG_MODULE:?set AIMEE_CONFIG_MODULE}
WFE=${AIMEE_WFE_BIN:?set AIMEE_WFE_BIN}
ROOT=${AIMEE_CONFIG_E2E_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/aimee-config-e2e.XXXXXX")}
mkdir -p "$ROOT"
export AIMEE_HOME="$ROOT/home"
mkdir -p "$AIMEE_HOME/modules.d/server" "$AIMEE_HOME/workflows"
chmod 0700 "$AIMEE_HOME" "$AIMEE_HOME/modules.d" "$AIMEE_HOME/modules.d/server"

HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
WFE_SOCK="$AIMEE_HOME/aimee-wfe.sock"
DB="$AIMEE_HOME/aimee.db"
CONFIG_FILE="$AIMEE_HOME/aimee.yaml"

cat >"$CONFIG_FILE" <<'EOF'
provider: codex
autonomy:
  max_turns: 333
EOF
chmod 0600 "$CONFIG_FILE"

write_grant() {
   file=$1
   ref=$2
   executable=$3
   request=$4
   serve=$5
   {
      echo "version=1"
      echo "principal_class=1"
      echo "principal_ref=$ref"
      echo "uid=self"
      echo "executable=$executable"
      echo "publish="
      echo "subscribe="
      echo "request=$request"
      echo "serve=$serve"
   } >"$file"
   chmod 0600 "$file"
}

write_grant "$AIMEE_HOME/modules.d/server/db1.grant" 30 "$DB1" "" \
   "11777,11778,11779,11780,11781,11782,11783,11784,11785,11786,11787,11788,11789,11790,11791,11792,11793,11794,11795"
write_grant "$AIMEE_HOME/modules.d/server/config.grant" 2 "$CONFIG" "" "4609"
write_grant "$AIMEE_HOME/modules.d/server/wfe.grant" 64 "$WFE" \
   "4609,6657,6678,9474,11792" ""

PIDS=""
cleanup() {
   [ -z "$PIDS" ] || kill $PIDS 2>/dev/null || true
}
trap cleanup EXIT INT TERM

wait_socket() {
   socket=$1
   count=0
   while [ "$count" -lt 300 ]; do
      [ -S "$socket" ] && return 0
      sleep 0.1
      count=$((count + 1))
   done
   return 1
}

wait_http() {
   socket=$1
   path=$2
   count=0
   while [ "$count" -lt 300 ]; do
      if curl -fsS --unix-socket "$socket" "http://localhost$path" >/dev/null 2>&1; then
         return 0
      fi
      sleep 0.1
      count=$((count + 1))
   done
   return 1
}

"$SERVER" --foreground >"$ROOT/server.log" 2>&1 &
SERVER_PID=$!
PIDS="$PIDS $SERVER_PID"
wait_socket "$BUS_SOCK"

AIMEE_DB1_PATH="$DB" "$DB1" "$BUS_SOCK" >"$ROOT/db1.log" 2>&1 &
DB1_PID=$!
PIDS="$PIDS $DB1_PID"
"$CONFIG" "$BUS_SOCK" >"$ROOT/config.log" 2>&1 &
CONFIG_PID=$!
PIDS="$PIDS $CONFIG_PID"

AIMEE_MODULE_BUS_SOCKET="$BUS_SOCK" "$WFE" --home "$AIMEE_HOME" \
   --socket "$WFE_SOCK" --workflow-dir "$AIMEE_HOME/workflows" \
   >"$ROOT/wfe.log" 2>&1 &
WFE_PID=$!
PIDS="$PIDS $WFE_PID"
wait_http "$WFE_SOCK" /v1/health

get_config() {
   curl -fsS --unix-socket "$WFE_SOCK" http://localhost/v1/config
}
set_config() {
   body=$1
   curl -sS --unix-socket "$WFE_SOCK" -H 'Content-Type: application/json' \
      -H 'X-Aimee-Workflow-Operator: true' -X POST -d "$body" \
      -w '\n%{http_code}' http://localhost/v1/config/set
}
http_status() {
   printf '%s\n' "$1" | tail -n 1
}

INITIAL=$(get_config)
printf '%s' "$INITIAL" | grep -q '"autonomy.max_turns":333'
printf 'PASS initial YAML read crossed the event bus\n'

UNAUTHORIZED=$(curl -sS --unix-socket "$WFE_SOCK" -H 'Content-Type: application/json' \
   -X POST -d '{"key":"autonomy.max_turns","value":1}' -w '\n%{http_code}' \
   http://localhost/v1/config/set)
[ "$(http_status "$UNAUTHORIZED")" = "403" ]
printf 'PASS unauthenticated mutation was rejected before the module call\n'

UPDATED=$(set_config '{"key":"autonomy.max_turns","value":444}')
[ "$(http_status "$UPDATED")" = "200" ]
get_config | grep -q '"autonomy.max_turns":444'
grep -q 'max_turns: 444' "$CONFIG_FILE"
printf 'PASS mutation crossed the bus and persisted atomically\n'

VERSION=$(curl -fsS --unix-socket "$WFE_SOCK" http://localhost/v1/workflow/triggers |
   python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
[ -n "$VERSION" ]
RULE='[{"source":"watch-dir","event":"docs/proposals/pending","mode":"autonomous","pipeline":{"template":"default","workspace":"/tmp/workspace"}}]'
BODY=$(printf '{"key":"trigger_rules","value":%s,"previous_version":"%s"}' "$RULE" "$VERSION")
FIRST=$(set_config "$BODY")
[ "$(http_status "$FIRST")" = "200" ]
STALE=$(set_config "$BODY")
[ "$(http_status "$STALE")" = "409" ]
printf '%s' "$STALE" | grep -q 'version conflict'
printf 'PASS optimistic version conflict survived both contract boundaries\n'

BEFORE=$(sha256sum "$CONFIG_FILE" | cut -d' ' -f1)
INVALID=$(set_config '{"key":"not.editable","value":true}')
[ "$(http_status "$INVALID")" = "400" ]
AFTER=$(sha256sum "$CONFIG_FILE" | cut -d' ' -f1)
[ "$BEFORE" = "$AFTER" ]
printf 'PASS invalid mutation returned typed failure without changing storage\n'

kill "$CONFIG_PID"
wait "$CONFIG_PID" 2>/dev/null || true
PIDS=$(printf '%s' "$PIDS" | sed "s/ $CONFIG_PID//")
DOWN=$(curl -sS --max-time 8 --unix-socket "$WFE_SOCK" -o "$ROOT/down.json" \
   -w '%{http_code}' http://localhost/v1/config || true)
[ "$DOWN" != "200" ]
printf 'PASS caller did not fall back to direct file access while module was down\n'

"$CONFIG" "$BUS_SOCK" >>"$ROOT/config.log" 2>&1 &
CONFIG_PID=$!
PIDS="$PIDS $CONFIG_PID"
count=0
while [ "$count" -lt 100 ]; do
   if get_config 2>/dev/null | grep -q '"autonomy.max_turns":444'; then
      break
   fi
   sleep 0.1
   count=$((count + 1))
done
[ "$count" -lt 100 ]
printf 'PASS restart reattached to the bus and preserved configuration\n'

BURST=${AIMEE_CONFIG_E2E_CONCURRENCY:-8}
n=1
WRITE_PIDS=""
while [ "$n" -le "$BURST" ]; do
   (
      RESULT=$(set_config "{\"key\":\"autonomy.max_turns\",\"value\":$((500 + n))}")
      printf '%s\n' "$RESULT" >"$ROOT/write-$n.out"
      case "$(http_status "$RESULT")" in
      200|503) ;;
      *) exit 1 ;;
      esac
   ) &
   WRITE_PIDS="$WRITE_PIDS $!"
   n=$((n + 1))
done
WRITE_FAILURE=0
for pid in $WRITE_PIDS; do
   if ! wait "$pid"; then
      WRITE_FAILURE=1
   fi
done
[ "$WRITE_FAILURE" -eq 0 ] || {
   echo "concurrent mutation failed" >&2
   for output in "$ROOT"/write-*.out; do
      printf '%s: ' "$output" >&2
      tail -n 1 "$output" >&2
   done
   exit 1
}
SUCCEEDED=0
BACKPRESSURE=0
for output in "$ROOT"/write-*.out; do
   case "$(tail -n 1 "$output")" in
   200) SUCCEEDED=$((SUCCEEDED + 1)) ;;
   503) BACKPRESSURE=$((BACKPRESSURE + 1)) ;;
   esac
done
[ "$SUCCEEDED" -gt 0 ]
if [ "$BURST" -le 16 ]; then
   [ "$BACKPRESSURE" -eq 0 ]
fi
FINAL=$(get_config)
printf '%s' "$FINAL" | grep -Eq '"autonomy.max_turns":(50[1-9]|51[0-9]|52[0-4])'
FINAL_FILE=$(sed -n 's/^[[:space:]]*max_turns:[[:space:]]*//p' "$CONFIG_FILE")
[ "$FINAL_FILE" -ge 501 ] && [ "$FINAL_FILE" -le $((500 + BURST)) ]
printf 'PASS %s concurrent bus mutations: %s persisted, %s received explicit backpressure\n' \
   "$BURST" "$SUCCEEDED" "$BACKPRESSURE"

cp "$CONFIG_FILE" "$ROOT/valid-config.yaml"
printf 'autonomy: [\n' >"$CONFIG_FILE"
BROKEN=$(curl -sS --unix-socket "$WFE_SOCK" -o "$ROOT/broken.json" -w '%{http_code}' \
   http://localhost/v1/config)
[ "$BROKEN" = "500" ]
cp "$ROOT/valid-config.yaml" "$CONFIG_FILE"
wait_http "$WFE_SOCK" /v1/config
printf 'PASS malformed out-of-band YAML failed closed and recovered in place\n'

[ "$(stat -c '%a' "$CONFIG_FILE")" = "600" ]
if find "$AIMEE_HOME" -maxdepth 1 -name '.aimee.yaml.*.tmp' -print -quit | grep -q .; then
   echo "temporary config file leaked" >&2
   exit 1
fi
printf 'PASS persistence retained mode 0600 and leaked no temporary file\n'

printf 'PASS config module contract E2E (%s)\n' "$ROOT"
