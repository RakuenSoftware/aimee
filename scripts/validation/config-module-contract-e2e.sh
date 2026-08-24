#!/bin/sh
# Real-process contract test: native server caller <-> event bus <-> pure-Go
# config provider. This uses the shipping server routes, not a second test API.
set -eu

SERVER=${AIMEE_SERVER_BIN:?set AIMEE_SERVER_BIN}
DB1=${AIMEE_DB1_MODULE:?set AIMEE_DB1_MODULE}
CONFIG=${AIMEE_CONFIG_MODULE:?set AIMEE_CONFIG_MODULE}
ROOT=${AIMEE_CONFIG_E2E_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/aimee-config-e2e.XXXXXX")}
mkdir -p "$ROOT"
export AIMEE_HOME="$ROOT/home"
POLICY="$AIMEE_HOME/modules.d/server"
mkdir -p "$POLICY"
chmod 0700 "$AIMEE_HOME" "$AIMEE_HOME/modules.d" "$POLICY"

HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
SERVER_SOCK="$AIMEE_HOME/aimee-server.sock"
DB="$AIMEE_HOME/aimee.db"
CONFIG_FILE="$AIMEE_HOME/aimee.yaml"

cat >"$CONFIG_FILE" <<'EOF'
provider: codex
max_iterations: 333
EOF
chmod 0600 "$CONFIG_FILE"

write_grant() {
   file=$1 ref=$2 executable=$3 serve=$4
   {
      echo "version=1"
      echo "principal_class=1"
      echo "principal_ref=$ref"
      echo "uid=self"
      echo "executable=$executable"
      echo "publish="
      echo "subscribe="
      echo "request="
      echo "serve=$serve"
   } >"$file"
   chmod 0600 "$file"
}

write_grant "$POLICY/aimee.grant" 30 "$DB1" \
   "11777,11778,11779,11780,11781,11782,11783,11784,11785,11786,11787,11788,11789,11790,11791,11792,11793,11794,11795"
write_grant "$POLICY/config.grant" 2 "$CONFIG" "4609"

PIDS=""
cleanup() {
   [ -z "$PIDS" ] || kill $PIDS 2>/dev/null || true
}
trap cleanup EXIT INT TERM

wait_socket() {
   socket=$1 count=0
   while [ "$count" -lt 300 ]; do
      [ -S "$socket" ] && return 0
      sleep 0.1
      count=$((count + 1))
   done
   return 1
}

config_get() {
   curl -fsS --unix-socket "$HTTP_SOCK" http://localhost/v1/config
}

config_set() {
   curl -fsS --unix-socket "$HTTP_SOCK" -H 'Content-Type: application/json' \
      -X POST -d "$1" http://localhost/v1/config/set
}

AIMEE_MODULE_POLICY_DIR="$POLICY" AIMEE_DB1_URL="sqlite://$DB" \
   "$SERVER" --socket="$SERVER_SOCK" >"$ROOT/server.log" 2>&1 &
SERVER_PID=$!
PIDS="$PIDS $SERVER_PID"
wait_socket "$BUS_SOCK"

AIMEE_MODULE_POLICY_DIR="$POLICY" AIMEE_STORE_URL="${AIMEE_STORE_URL:-}" \
   "$DB1" "$BUS_SOCK" >"$ROOT/db1.log" 2>&1 &
DB1_PID=$!
PIDS="$PIDS $DB1_PID"
AIMEE_MODULE_POLICY_DIR="$POLICY" "$CONFIG" "$BUS_SOCK" >"$ROOT/config.log" 2>&1 &
CONFIG_PID=$!
PIDS="$PIDS $CONFIG_PID"
wait_socket "$HTTP_SOCK"

INITIAL=$(config_get)
printf '%s' "$INITIAL" | grep -q '"max_iterations":333'
printf 'PASS initial YAML read crossed the event bus\n'

UPDATED=$(config_set '{"key":"max_iterations","value":444}')
printf '%s' "$UPDATED" | grep -q '"status":"ok"'
config_get | grep -q '"max_iterations":444'
grep -q '^max_iterations: 444' "$CONFIG_FILE"
printf 'PASS mutation crossed the bus and persisted atomically\n'

CREATED=$(config_set '{"operation":"profile-create","value":{"name":"contract-profile"}}')
printf '%s' "$CREATED" | grep -q '"status":"ok"'
[ -f "$AIMEE_HOME/profiles/contract-profile/aimee.yaml" ]
[ "$(stat -c '%a' "$AIMEE_HOME/profiles/contract-profile/aimee.yaml")" = "600" ]
PRESENT=$(config_set '{"operation":"profile-present","value":{"name":"contract-profile"}}')
printf '%s' "$PRESENT" | grep -q '"present":true'
LISTED=$(config_set '{"operation":"profile-list"}')
printf '%s' "$LISTED" | grep -q '"profiles":\["contract-profile"\]'
DELETED=$(config_set '{"operation":"profile-delete","value":{"name":"contract-profile"}}')
printf '%s' "$DELETED" | grep -q '"status":"ok"'
[ ! -e "$AIMEE_HOME/profiles/contract-profile" ]
ABSENT=$(config_set '{"operation":"profile-present","value":{"name":"contract-profile"}}')
printf '%s' "$ABSENT" | grep -q '"present":false'
printf 'PASS complete profile lifecycle crossed the bidirectional bus contract\n'

BEFORE=$(sha256sum "$CONFIG_FILE" | cut -d' ' -f1)
INVALID=$(config_set '{"key":"not.editable","value":true}')
printf '%s' "$INVALID" | grep -q '"status":"error"'
AFTER=$(sha256sum "$CONFIG_FILE" | cut -d' ' -f1)
[ "$BEFORE" = "$AFTER" ]
printf 'PASS invalid mutation returned a typed failure without changing storage\n'

kill "$CONFIG_PID"
wait "$CONFIG_PID" 2>/dev/null || true
PIDS=$(printf '%s' "$PIDS" | sed "s/ $CONFIG_PID//")
DOWN=$(config_get)
printf '%s' "$DOWN" | grep -q '"status":"error"'
printf 'PASS caller did not fall back to direct file access while provider was down\n'

AIMEE_MODULE_POLICY_DIR="$POLICY" "$CONFIG" "$BUS_SOCK" >>"$ROOT/config.log" 2>&1 &
CONFIG_PID=$!
PIDS="$PIDS $CONFIG_PID"
count=0
while [ "$count" -lt 100 ]; do
   if config_get 2>/dev/null | grep -q '"max_iterations":444'; then
      break
   fi
   sleep 0.1
   count=$((count + 1))
done
[ "$count" -lt 100 ]
printf 'PASS provider restart reattached and preserved configuration\n'

BURST=${AIMEE_CONFIG_E2E_CONCURRENCY:-8}
n=1
WRITE_PIDS=""
while [ "$n" -le "$BURST" ]; do
   (
      config_set "{\"key\":\"max_iterations\",\"value\":$((500 + n))}" \
         >"$ROOT/write-$n.out"
   ) &
   WRITE_PIDS="$WRITE_PIDS $!"
   n=$((n + 1))
done
for pid in $WRITE_PIDS; do wait "$pid"; done
for output in "$ROOT"/write-*.out; do grep -q '"status":"ok"' "$output"; done
FINAL_FILE=$(sed -n 's/^max_iterations:[[:space:]]*//p' "$CONFIG_FILE")
[ "$FINAL_FILE" -ge 501 ] && [ "$FINAL_FILE" -le $((500 + BURST)) ]
printf 'PASS %s concurrent bus mutations all persisted without corruption\n' "$BURST"

cp "$CONFIG_FILE" "$ROOT/valid-config.yaml"
printf 'autonomy: [\n' >"$CONFIG_FILE"
BROKEN=$(config_set '{"key":"max_iterations","value":777}')
printf '%s' "$BROKEN" | grep -q '"status":"error"'
cp "$ROOT/valid-config.yaml" "$CONFIG_FILE"
count=0
while [ "$count" -lt 100 ]; do
   config_get 2>/dev/null | grep -q '"status":"ok"' && break
   sleep 0.1
   count=$((count + 1))
done
[ "$count" -lt 100 ]
printf 'PASS malformed out-of-band YAML failed closed and recovered in place\n'

[ "$(stat -c '%a' "$CONFIG_FILE")" = "600" ]
if find "$AIMEE_HOME" -maxdepth 1 -name '.aimee.yaml.*.tmp' -print -quit | grep -q .; then
   echo "temporary config file leaked" >&2
   exit 1
fi
printf 'PASS persistence retained mode 0600 and leaked no temporary file\n'
printf 'PASS config module contract E2E (%s)\n' "$ROOT"
