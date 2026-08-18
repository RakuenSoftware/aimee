#!/bin/bash
# test_init_migrate_service.sh: client -> server -> KB-owner routing smoke test.
# Uses a fake sibling aimee-kb helper so no local Postgres privileges are required.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

AIMEE="${AIMEE_CLIENT:-$REPO_ROOT/aimee}"
AIMEE_SERVER="${AIMEE_SERVER:-$REPO_ROOT/aimee-server}"
export HOME=$(mktemp -d /tmp/aimee-init-migrate-XXXXXX)
export AIMEE_HOME="$HOME/.config/aimee"
unset AIMEE_PROFILE
SERVICE_BIN_DIR="$HOME/service-bin"
SERVICE_SERVER="$SERVICE_BIN_DIR/aimee-server"
SOCKET="$AIMEE_HOME/aimee.sock"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
export AIMEE_SOCK="$SOCKET"
# The client reaches a server ONLY through an explicitly configured endpoint —
# it no longer discovers a co-located one by probing the filesystem, because a
# stray local server silently answering for the wrong host is worse than an
# outage. This harness deliberately runs both halves on one box, so it has to
# say so. Same form the server itself uses when it spawns delegate CLIs
# (server/provider_cli_adapter.c).
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"
SERVER_PID=""
PASS=0
FAIL=0

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$HOME"
}
trap cleanup EXIT

require_binary() {
    if [ ! -x "$1" ]; then
        echo "missing test prerequisite: $1"
        exit 1
    fi
}

check() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

check_output() {
    local desc="$1"
    local expected="$2"
    shift 2
    local output
    output=$("$@" 2>&1) || true
    if echo "$output" | grep -qF "$expected"; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected '$expected', got '$(echo "$output" | head -1)')"
        FAIL=$((FAIL + 1))
    fi
}

# The NDJSON RPC socket and the /v1/rpc bridge were both removed; every method is
# now reached over its first-class /v1 route. This helper POSTs a raw request to a
# given path over the local HTTP UDS (filesystem-trusted, no token) and prints the
# numeric HTTP status, so the retirement of /v1/rpc can be asserted directly.
srv_status() {
    python3 -c "
import socket, sys
path, body = sys.argv[1], sys.argv[2]
req = ('POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n'
       'Content-Length: %d\r\nConnection: close\r\n\r\n%s' % (path, len(body), body))
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$HTTP_SOCK')
s.settimeout(30)
s.sendall(req.encode())
data = b''
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
s.close()
status = data.split(b' ', 2)[1].decode() if data.startswith(b'HTTP/') else '000'
sys.stdout.write(status)
" "$1" "$2" 2>/dev/null
}

require_binary "$AIMEE"
require_binary "$AIMEE_SERVER"

mkdir -p "$AIMEE_HOME" "$SERVICE_BIN_DIR"
cp "$AIMEE_SERVER" "$SERVICE_SERVER"
cat > "$SERVICE_BIN_DIR/aimee-kb" <<'EOF'
#!/bin/sh
case "$1" in
  --bootstrap-db2)
    printf '%s\n' '{"status":"ok","bootstrapped":true,"db2_url_saved":true}'
    ;;
  --migrate-db2-from-sqlite)
    printf '{"status":"ok","source":"%s","tables_found":1,"source_rows":2,' "$2"
    printf '"rows_copied":2,"rows_skipped":0,"table_errors":0,'
    printf '"row_count_regressions":0,"backup_path":"%s.pre1"}\n' "$2"
    ;;
  *)
    printf '%s\n' '{"status":"error","message":"unexpected fake kb arguments"}'
    exit 2
    ;;
esac
EOF
chmod +x "$SERVICE_SERVER" "$SERVICE_BIN_DIR/aimee-kb"


"$SERVICE_SERVER" --foreground >/dev/null 2>&1 &
SERVER_PID=$!

# Wait for the server to actually bind its HTTP socket instead of guessing with a
# fixed `sleep 1`. On a loaded CI runner startup regularly takes several seconds,
# and the 1s guess made this the run's flakiest test ("service route server
# started" / "client ... server unavailable"). Poll for the socket (~30s cap) and
# bail early if the server process dies so a crash fails fast instead of timing
# out. A unix-socket server binds+listens as the last init step, so socket-present
# means ready to serve.
for _ in $(seq 1 300); do
    [ -S "$HTTP_SOCK" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
done

check "service route server started" test -S "$HTTP_SOCK"
check_output "client init routes through server to kb owner" '"knowledge_ready":true' \
    "$AIMEE" --json init

LEGACY_SHARED="$HOME/legacy-shared.db"
: > "$LEGACY_SHARED"

# The /v1/rpc bridge is retired: there is no generic dispatch entry point, so the
# unexposed migrate aliases (migrate.db2_to_postgres / migrate.v2) are unreachable
# by construction. Assert the bridge itself is gone — the path now 404s.
STATUS=$(srv_status /v1/rpc '{"method":"migrate.db2_to_postgres","source_path":"legacy-shared.db"}')
check_output "/v1/rpc bridge is retired (404)" '404' echo "$STATUS"

STATUS=$(srv_status /v1/rpc '{"method":"migrate.v2","source_path":"legacy-shared.db"}')
check_output "no generic dispatch bridge remains (404)" '404' echo "$STATUS"

TOTAL=$((PASS + FAIL))
echo ""
echo "init-migrate-service: $PASS/$TOTAL passed"
if [ "$FAIL" -gt 0 ]; then
    echo "FAILED ($FAIL failures)"
    exit 1
fi
echo "All init/migrate service routing tests passed."
