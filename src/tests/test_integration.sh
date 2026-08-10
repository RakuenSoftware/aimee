#!/bin/bash
# test_integration.sh: smoke tests for the aimee server architecture
# Run from the repo root: ./src/tests/test_integration.sh
#
# Tests the full client -> server -> response path including:
# - Server lifecycle (start, health, shutdown)
# - Authentication and capabilities
# - Session management
# - Memory CRUD via server
# - Hooks through server
# - CLI forwarding fail-closed behavior for commands without native RPC routes
# - Tool execution via compute pool
# - Graceful degradation (no server)

set -e

# Resolve paths relative to repo root
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

AIMEE="$REPO_ROOT/aimee"
AIMEE_SERVER="$REPO_ROOT/aimee-server"
export PATH="$REPO_ROOT:$PATH"
export HOME=$(mktemp -d /tmp/aimee-integ-XXXXXX)
export AIMEE_HOME="$HOME/.config/aimee"
unset AIMEE_PROFILE
mkdir -p "$AIMEE_HOME"
SOCKET="$AIMEE_HOME/aimee.sock"
export AIMEE_SOCK="$SOCKET"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"

PASS=0
FAIL=0
SKIP=0

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

check_exit() {
    local desc="$1"
    local expected_code="$2"
    shift 2
    "$@" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" -eq "$expected_code" ]; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc (expected exit $expected_code, got $rc)"
        FAIL=$((FAIL + 1))
    fi
}

require_binary "$AIMEE"
require_binary "$AIMEE_SERVER"

# The NDJSON RPC socket and the /v1/rpc bridge were both removed: aimee-server is
# reached only over its first-class /v1 HTTP routes. srv_req / srv_auth_req are
# kept as thin aliases over the http_rpc helper (defined below), which maps each
# {method} to its dedicated route, so the existing assertions keep working — the
# local HTTP UDS is filesystem-trusted, so no separate auth step is needed.
srv_req() {
    http_rpc "$1"
}

srv_auth_req() {
    http_rpc "$1"
}

# Helper: dispatch a {method,...} body to its first-class /v1 route over the HTTP
# UDS socket; print the raw response body. (/v1/rpc was retired — each method has
# a dedicated route. The body is sent on GET too: the server reads it via
# Content-Length regardless of verb, so dispatch routes get their params.)
http_rpc() {
    python3 -c "
import socket, sys, json
body = sys.argv[1]
method = (json.loads(body).get('method') or '')
routes = {
  'server.info': ('GET', '/v1/server/info'),
  'server.health': ('GET', '/v1/server/health'),
  'provider.list': ('GET', '/v1/provider/list'),
  'workspace.add': ('POST', '/v1/workspaces'),
  'rules.list': ('GET', '/v1/rules'),
  'session.list': ('POST', '/v1/sessions/list'),
  'memory.list': ('POST', '/v1/memory/list'),
  'memory.store': ('POST', '/v1/memory/store'),
  'memory.get': ('POST', '/v1/memory/get'),
  'tool.execute': ('POST', '/v1/tools/execute'),
  'session.create': ('POST', '/v1/sessions/create'),
  'session.get': ('POST', '/v1/sessions/get'),
  'session.close': ('POST', '/v1/sessions/close'),
}
verb, path = routes.get(method, ('POST', '/v1/' + method.replace('.', '/')))
req = ('%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n'
       'Content-Length: %d\r\nConnection: close\r\n\r\n%s' % (verb, path, len(body), body))
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$HTTP_SOCK')
s.settimeout(10)
s.sendall(req.encode())
data = b''
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
s.close()
_, _, payload = data.partition(b'\r\n\r\n')
sys.stdout.write(payload.decode().strip())
" "$1" 2>/dev/null
}

mcp_framed_req() {
    python3 - "$AIMEE" "$1" <<'PY'
import subprocess
import sys

cmd = [sys.argv[1], "mcp-serve"]
payload = sys.argv[2].encode()
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def recv():
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = p.stdout.read(1)
        if not chunk:
            raise SystemExit(1)
        header += chunk
    raw_headers, body = header.split(b"\r\n\r\n", 1)
    length = None
    for line in raw_headers.decode().split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
            break
    if length is None:
        raise SystemExit(1)
    while len(body) < length:
        chunk = p.stdout.read(length - len(body))
        if not chunk:
            raise SystemExit(1)
        body += chunk
    return body[:length].decode()

p.stdin.write(f"Content-Length: {len(payload)}\r\n\r\n".encode() + payload)
p.stdin.flush()
print(recv())
p.terminate()
try:
    p.wait(timeout=2)
except subprocess.TimeoutExpired:
    p.kill()
PY
}

mcp_initialized_req() {
    python3 - "$AIMEE" "$1" <<'PY'
import subprocess
import sys

cmd = [sys.argv[1], "mcp-serve"]
request = sys.argv[2]
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def send(payload):
    body = payload.encode()
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    p.stdin.flush()

def recv():
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = p.stdout.read(1)
        if not chunk:
            raise SystemExit(1)
        header += chunk
    raw_headers, body = header.split(b"\r\n\r\n", 1)
    length = None
    for line in raw_headers.decode().split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
            break
    if length is None:
        raise SystemExit(1)
    while len(body) < length:
        chunk = p.stdout.read(length - len(body))
        if not chunk:
            raise SystemExit(1)
        body += chunk
    return body[:length].decode()

send('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"integration-test","version":"1"}}}')
recv()
send('{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}')
send(request)
print(recv())
p.terminate()
try:
    p.wait(timeout=2)
except subprocess.TimeoutExpired:
    p.kill()
PY
}

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$HOME"
}
trap cleanup EXIT

# ============================================================
# Setup: server initializes DB1 on startup
# ============================================================

# ============================================================
# 1. Auto-start (no server running)
# ============================================================

check_output "no server: version works" "aimee" $AIMEE version

# Disable auto-start for all server tests (they manage the server explicitly)
export AIMEE_NO_AUTOSTART=1

# Pure clients do not run command handlers locally.
check_output "no server: DB command fails without autostart" "server unavailable" $AIMEE memory list

# ============================================================
# 2. Server lifecycle
# ============================================================

$AIMEE_SERVER --foreground >/dev/null 2>&1 &
SERVER_PID=$!
sleep 1

check "server started" test -S "$HTTP_SOCK"

RESP=$(srv_req '{"method":"server.info"}')
check_output "server.info status" '"status":"ok"' echo "$RESP"
check_output "server.info version" '"server_version"' echo "$RESP"
check_output "server.info method list" '"methods"' echo "$RESP"
check_output "server.info delegate status method" '"delegate.status"' echo "$RESP"
check_output "server.info exposes launch.run" '"launch.run"' echo "$RESP"

# Bare `aimee` is deliberately lightweight and prints discoverable help. A
# noninteractive bare command must not accidentally start the retired TUI/provider
# path; interactive clients use the ACP/editor surfaces.
check_output "no-arg aimee prints usage" 'Usage: aimee' $AIMEE --json
check_output "no-arg aimee prints the command catalog" 'Commands:' $AIMEE --json

# api command: status must reach the server and print a report (regression —
# api.status was registered as a route + handler but missing from the RPC
# marshaler, so `aimee api status` silently returned exit 2 with no output),
# and enable/disable must round-trip the aimee.api.* config.
check_output "api status reports the listener" "listener:" $AIMEE api status
check_output "api status starts disabled" "disabled" $AIMEE api status
check_output "api enable turns the listener on" "enabled on" $AIMEE api enable
check_output "api enable persisted the port" "8910" $AIMEE api status
check_output "api disable turns the listener off" "disabled" $AIMEE api disable
check_output "no-arg path remains local help" 'Server is started automatically' $AIMEE --json

RESP=$(srv_req '{"method":"server.health"}')
check_output "server.health status" '"status":"ok"' echo "$RESP"
check_output "server.health uptime" '"uptime"' echo "$RESP"

# The /v1 HTTP surface is the only transport now (the NDJSON RPC socket was
# removed). Confirm the local /v1 UDS is bound and an allowlisted read returns a
# well-formed dispatch response over its first-class /v1 route.
if [ -S "$HTTP_SOCK" ]; then
    PASS=$((PASS + 1)) # /v1 HTTP socket bound
    HTTP_PROV=$(http_rpc '{"method":"provider.list"}')
    if [ -n "$HTTP_PROV" ] && python3 -c "import json,sys; json.loads(sys.argv[1])" "$HTTP_PROV" \
        2>/dev/null; then
        PASS=$((PASS + 1)) # provider.list over /v1/provider/list returned valid JSON
    else
        echo "FAIL: provider.list over /v1/provider/list returned no usable body"
        echo "  http: $HTTP_PROV"
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: /v1 HTTP socket not bound at $HTTP_SOCK"
    FAIL=$((FAIL + 1))
fi

# HTTP/UDS vs HTTP/TCP byte-identity: the /v1 surface must return the exact same
# bytes whether reached over the always-on Unix socket or the optional localhost
# TCP listener (gated by aimee.api.{http_port,bearer_token}). Use an isolated
# second server so the main harness server (UDS-only) is undisturbed.
TCP_HOME=$(mktemp -d /tmp/aimee-tcp-XXXXXX)
mkdir -p "$TCP_HOME/.config/aimee"
TCP_PORT=18897
TCP_BEARER="integ-tcp-bearer"
printf 'aimee:\n  api:\n    http_port: %s\n    bearer_token: %s\n' \
    "$TCP_PORT" "$TCP_BEARER" >"$TCP_HOME/.config/aimee/aimee.yaml"
env -u AIMEE_PROFILE HOME="$TCP_HOME" AIMEE_HOME="$TCP_HOME/.config/aimee" \
    AIMEE_DISABLE_AUTOSTART=1 "$AIMEE_SERVER" --foreground >/dev/null 2>&1 &
TCP_SRV_PID=$!
sleep 2
UDS_BODY=$(HTTP_SOCK="$TCP_HOME/.config/aimee/aimee-http.sock" http_rpc '{"method":"provider.list"}')
TCP_BODY=$(python3 -c "
import socket, sys
body = '{\"method\":\"provider.list\"}'
req = ('GET /v1/provider/list HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer $TCP_BEARER\r\n'
       'Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s'
       % (len(body), body))
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(10)
try:
    s.connect(('127.0.0.1', $TCP_PORT))
    s.sendall(req.encode())
    data = b''
    while True:
        c = s.recv(4096)
        if not c:
            break
        data += c
    s.close()
    sys.stdout.write(data.partition(b'\r\n\r\n')[2].decode().strip())
except OSError:
    pass
" 2>/dev/null)
if [ -n "$UDS_BODY" ] && [ "$UDS_BODY" = "$TCP_BODY" ]; then
    PASS=$((PASS + 1)) # HTTP/UDS == HTTP/TCP byte-identical
else
    echo "FAIL: /v1 HTTP/UDS vs HTTP/TCP byte-identity"
    echo "  uds: $UDS_BODY"
    echo "  tcp: $TCP_BODY"
    FAIL=$((FAIL + 1))
fi
kill "$TCP_SRV_PID" 2>/dev/null || true
rm -rf "$TCP_HOME"

# Rebuild only the thin client with a new build ID. The existing server should
# remain usable because compatibility is gated by major version, not build ID.
SERVER_PID_BEFORE="$SERVER_PID"
(
    cd "$REPO_ROOT/src"
    sleep 1
    date +%s > build/build_id.txt
    make ../aimee >/dev/null 2>&1
)

check_output "client survives same-major build drift" "aimee" $AIMEE version
check "server survives same-major build drift" kill -0 "$SERVER_PID_BEFORE"

# If the compatibility check regressed and the client restarted the server,
# recover a known foreground server so the rest of the test stays meaningful.
if ! kill -0 "$SERVER_PID_BEFORE" 2>/dev/null; then
    pkill -f "aimee-server.*$SOCKET" 2>/dev/null || true
    $AIMEE_SERVER --foreground >/dev/null 2>&1 &
    SERVER_PID=$!
    sleep 1
fi

check_output "local version long flag" "aimee" $AIMEE --version
check "index overview route" $AIMEE index overview

RESP=$(mcp_framed_req '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"integration-test","version":"1"}}}')
check_output "mcp initialize over stdio framing" '"protocolVersion":"2024-11-05"' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":2,"method":"prompts/get","params":{"name":"search-and-summarize","arguments":{"query":"mcp"}}}')
check_output "mcp prompts/get" '"messages"' echo "$RESP"
check_output "mcp prompts/get text" 'search_memory' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":3,"method":"resources/templates/list","params":{}}')
check_output "mcp resources/templates/list" '"resourceTemplates"' echo "$RESP"
check_output "mcp resources/templates/list tier template" 'aimee://memories/{tier}' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"aimee://config"}}')
check_output "mcp resources/read config" '"mimeType":"application/json"' echo "$RESP"
check_output "mcp resources/read config text" '\"protocolVersion\":\"2024-11-05\"' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"git_status","arguments":{}}}')
check_output "mcp git_status tool call" '"content"' echo "$RESP"
check_output "mcp git_status result text" 'branch:' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"get_help","arguments":{}}}')
check_output "mcp get_help tool call" '"content"' echo "$RESP"
check_output "mcp get_help topic index" 'Aimee delegate reference' echo "$RESP"

# ============================================================
# 3. Local /v1 trust model
# ============================================================
# The NDJSON attestation/token handshake was removed. The local /v1 UDS is
# filesystem-permission gated and fully trusted: it reaches the entire dispatch
# surface (reads AND writes) with no token. (The optional TCP listener is the
# bearer-scoped path, exercised separately below.)

# Reads work when a knowledge store is wired. This integration target starts
# only aimee-server; full server+kb coverage lives in aimee-local-stack-e2e.sh
# and the Docker deploy matrix. Keep the local transport checks useful on hosts
# without Postgres instead of reporting an expected dependency absence as a
# server regression.
RESP=$(srv_req '{"method":"memory.list","limit":1}')
if echo "$RESP" | grep -qF '"status":"ok"'; then
    KB_AVAILABLE=1
    PASS=$((PASS + 1))
    RESP=$(srv_req '{"method":"rules.list"}')
    check_output "local /v1: rules.list ok" '"status":"ok"' echo "$RESP"
else
    KB_AVAILABLE=0
    echo "SKIP: local /v1 memory/rules reads (aimee-kb is not configured)"
    SKIP=$((SKIP + 2))
fi

# A write/control method is reachable over the trusted local socket — it must NOT
# come back capability-denied ("not permitted over /v1").
RESP=$(srv_req '{"method":"tool.execute","tool":"bash","arguments":"{\"command\":\"echo hi\"}","session_id":"t","cwd":"/tmp","timeout_ms":1000}')
if echo "$RESP" | grep -q "not permitted over /v1"; then
    echo "FAIL: local /v1: tool.execute unexpectedly capability-gated"
    echo "  resp: $RESP"
    FAIL=$((FAIL + 1))
else
    PASS=$((PASS + 1)) # tool.execute reachable over the trusted local /v1 socket
fi

# ============================================================
# 4. Session management
# ============================================================

RESP=$(srv_auth_req '{"method":"session.create","client_type":"test"}')
check_output "session.create" '"status":"ok"' echo "$RESP"
SID=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['session_id'])" 2>/dev/null)

RESP=$(srv_auth_req '{"method":"session.list"}')
check_output "session.list has session" "$SID" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.get\",\"session_id\":\"$SID\"}")
check_output "session.get" '"client_type":"test"' echo "$RESP"

RESP=$($AIMEE session list 2>&1)
check_output "client session list via server" "$SID" echo "$RESP"

RESP=$($AIMEE session show "$SID" 2>&1)
check_output "client session show via server" "client:      test" echo "$RESP"

RESP=$($AIMEE --json session list --limit 1 2>&1)
check_output "client session list json" "$SID" echo "$RESP"

RESP=$(srv_auth_req '{"method":"session.create","client_type":"test-close"}')
check_output "session.create for client close" '"status":"ok"' echo "$RESP"
CLOSE_SID=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['session_id'])" 2>/dev/null)

RESP=$($AIMEE session close "$CLOSE_SID" 2>&1)
check_output "client session close via server" "$CLOSE_SID" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.get\",\"session_id\":\"$CLOSE_SID\"}")
check_output "client session close removed session" "session not found" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.close\",\"session_id\":\"$SID\"}")
check_output "session.close" '"status":"ok"' echo "$RESP"

RESP=$(srv_auth_req '{"method":"session.list"}')
check_output "session.list empty after close" '"sessions":[]' echo "$RESP"

# ============================================================
# 5. Memory via server
# ============================================================

if [ "$KB_AVAILABLE" -eq 1 ]; then
    RESP=$(srv_auth_req '{"method":"memory.store","key":"integ-test","content":"integration test value","tier":"L0","kind":"fact"}')
    check_output "memory.store" '"status":"ok"' echo "$RESP"
    MEM_ID=$(echo "$RESP" | python3 -c "import sys,json; print(int(json.load(sys.stdin)['id']))" 2>/dev/null)

    RESP=$(srv_auth_req '{"method":"memory.list","tier":"L0","limit":10}')
    check_output "memory.list has stored entry" "integ-test" echo "$RESP"

    RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$MEM_ID}")
    check_output "memory.get by ID" "integration test value" echo "$RESP"

    # `memory get --as-of` crosses client -> aimee-server -> aimee-kb, and it was
    # once broken in the middle: the server read only the id, so the flag was
    # marshalled, sent, and dropped, and the client printed the row with no
    # verdict -- which reads exactly like "not in force". Every unit test around
    # it passed, because each end was checked against a hand-written payload that
    # already contained the field. Only the real wire shows the gap, so assert it
    # here: the verdict must come back, and must NOT appear when nobody asked.
    RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$MEM_ID,\"as_of\":\"2020-01-01 00:00:00\"}")
    check_output "memory.get --as-of echoes the timestamp" '"as_of"' echo "$RESP"
    check_output "memory.get --as-of returns an event-time verdict" '"valid_at"' echo "$RESP"

    RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$MEM_ID}")
    if echo "$RESP" | grep -q '"valid_at"'; then
        check_output "memory.get without --as-of emits no verdict" "no valid_at" echo "found valid_at"
    else
        check_output "memory.get without --as-of emits no verdict" "ok" echo "ok"
    fi
else
    echo "SKIP: memory write/read round-trip (aimee-kb is not configured)"
    SKIP=$((SKIP + 6))
fi

# ============================================================
# 6. Hooks through server
# ============================================================

HOOK_PAYLOAD='{"tool_name":"Read","tool_input":"{\"file_path\":\"main.c\"}"}'
set +e
RESP=$(echo "$HOOK_PAYLOAD" | CLAUDE_SESSION_ID=integ-test $AIMEE hooks pre 2>&1)
HOOK_RC=$?
set -e
if [ "$HOOK_RC" -ne 0 ] && echo "$RESP" | grep -q "server build mismatch"; then
    pkill -f "aimee-server.*$SOCKET" 2>/dev/null || true
    (
        cd "$REPO_ROOT/src"
        make ../aimee-server >/dev/null 2>&1
    )
    $AIMEE_SERVER --foreground >/dev/null 2>&1 &
    SERVER_PID=$!
    sleep 1
    set +e
    RESP=$(echo "$HOOK_PAYLOAD" | CLAUDE_SESSION_ID=integ-test $AIMEE hooks pre 2>&1)
    HOOK_RC=$?
    set -e
fi
if [ "$HOOK_RC" -eq 0 ]; then
    PASS=$((PASS + 1))
else
    echo "FAIL: hooks pre safe file (expected exit 0, got $HOOK_RC)"
    FAIL=$((FAIL + 1))
fi

# ============================================================
# 7. Unported commands fail before any generic command forwarding
# ============================================================

check_output "local version command" "aimee" $AIMEE version
check_output "unknown command fails before generic forwarding" "unknown command 'env'" $AIMEE env

# ============================================================
# 8. Tool execution via compute pool
# ============================================================

# tool.execute runs on the compute pool, reached over its first-class local route
# POST /v1/tools/execute (the `arguments` field is a JSON-encoded string, as the
# dispatch expects).
RESP=$(http_rpc '{"method":"tool.execute","tool":"bash","arguments":"{\"command\":\"echo integ-ok\"}","session_id":"t","cwd":"/tmp","timeout_ms":5000}')
check_output "tool.execute bash" "integ-ok" echo "$RESP"

RESP=$(http_rpc '{"method":"tool.execute","tool":"read_file","arguments":"{\"path\":\"/etc/hostname\",\"limit\":1}","session_id":"t","cwd":"/tmp"}')
check_output "tool.execute read_file" '"status":"ok"' echo "$RESP"

# ============================================================
# 9. Mirror-sync -> reconstructed MCP Git, end to end
# ============================================================

MIRROR_CASE="$HOME/mirror-sync-e2e"
MIRROR_REMOTE="$MIRROR_CASE/remote.git"
MIRROR_CLIENT="$MIRROR_CASE/client"
mkdir -p "$MIRROR_CASE"
# Exercise Git's repository-format SHA-256 object IDs through the entire wire,
# snapshot, reconstruction, and MCP tool path (SHA-1 is covered by unit tests).
git init --bare --object-format=sha256 "$MIRROR_REMOTE" >/dev/null
git init --object-format=sha256 -b feature.locked "$MIRROR_CLIENT" >/dev/null
git -C "$MIRROR_CLIENT" config user.name "Aimee Integration"
git -C "$MIRROR_CLIENT" config user.email "aimee-integration@example.invalid"
printf 'base\n' >"$MIRROR_CLIENT/file.txt"
git -C "$MIRROR_CLIENT" add file.txt
git -C "$MIRROR_CLIENT" commit -m base >/dev/null
git -C "$MIRROR_CLIENT" remote add origin "$MIRROR_REMOTE"
git -C "$MIRROR_CLIENT" push -u origin feature.locked >/dev/null
MIRROR_HEAD=$(git -C "$MIRROR_CLIENT" rev-parse HEAD)
printf 'client edit\n' >>"$MIRROR_CLIENT/file.txt"
git -C "$MIRROR_CLIENT" diff --binary HEAD >"$MIRROR_CASE/client.diff"

ADD_REQ=$(python3 - "$MIRROR_CLIENT" "$MIRROR_REMOTE" "$MIRROR_HEAD" <<'PY'
import json, sys
print(json.dumps({"method": "workspace.add", "root": sys.argv[1], "provider": "mirror",
                  "remote": sys.argv[2], "head": sys.argv[3], "scan": False}))
PY
)
RESP=$(http_rpc "$ADD_REQ")
check_output "mirror workspace registered" '"status":"ok"' echo "$RESP"

# Deliberately split a small patch into multiple requests. The durable transfer
# state is re-read on every request, which is the same contract used when a load
# balancer or rolling deployment routes consecutive chunks to different server
# processes sharing AIMEE_WORKSPACES_DIR.
mapfile -t MIRROR_REQS < <(python3 - "$MIRROR_CLIENT" "$MIRROR_HEAD" \
    "$MIRROR_CASE/client.diff" <<'PY'
import json, pathlib, sys
root, head, path = sys.argv[1:]
patch = pathlib.Path(path).read_text()
cut = max(1, len(patch) // 2)
for transfer in ("0123456789abcdef0123456789abcdef",
                 "1123456789abcdef0123456789abcdef"):
    base = {"method": "workspace.mirror-sync", "args": [root], "transfer": transfer}
    print(json.dumps(base | {"seq": 0, "final": False, "diff": ""}))
    print(json.dumps(base | {"seq": 1, "final": False, "diff": patch[:cut]}))
    print(json.dumps(base | {"seq": 2, "final": True, "diff": patch[cut:], "head": head,
                             "branch": "feature.locked", "upstream": "origin/feature.locked"}))
PY
)
RESP=$(http_rpc "${MIRROR_REQS[0]}")
check_output "mirror-sync begin persisted" '"order":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[1]}")
check_output "mirror-sync continuation persisted" '"seq":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[2]}")
check_output "mirror-sync final published" '"generation":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[3]}")
check_output "identical mirror-sync begin advances order" '"order":2' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[4]}")
check_output "identical mirror-sync continuation persisted" '"seq":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[5]}")
check_output "identical mirror-sync reuses generation" '"generation":1' echo "$RESP"

SNAPSHOT=$(find "$AIMEE_HOME/workspaces" -name client.snapshot -type f -print -quit)
check "mirror snapshot metadata published" test -s "$SNAPSHOT"
check_output "mirror snapshot records valid .locked ref" \
    'feature.locked origin/feature.locked 2' cat "$SNAPSHOT"

GIT_REQ=$(python3 - "$MIRROR_CLIENT" <<'PY'
import json, sys
print(json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                  "params": {"name": "git", "arguments": {"command": "status",
                                                               "path": sys.argv[1]}}}))
PY
)
RESP=$(mcp_initialized_req "$GIT_REQ")
check_output "MCP Git resolves reconstructed .locked branch" 'feature.locked' echo "$RESP"
check_output "MCP Git sees synchronized client edit" 'file.txt' echo "$RESP"

MIRROR_WORK=$(find "$(dirname "$SNAPSHOT")" -maxdepth 1 -type d -name 'work-1-*' -print -quit)
check_output "reconstructed worktree keeps branch" 'feature.locked' \
    git -C "$MIRROR_WORK" branch --show-current
check_output "reconstructed worktree keeps dirty patch" 'file.txt' git -C "$MIRROR_WORK" status --short

# The second identical multi-chunk refresh must retain generation 1 (and
# therefore the same server-side Git checkout) while advancing publication
# order to prevent an older transfer rolling it back.
check_output "identical refresh reuses snapshot generation" '1 ' head -c 2 "$SNAPSHOT"
check_output "identical refresh advances publication order" ' 2' tail -c 3 "$SNAPSHOT"

# A clean checkout publishes an empty diff file. It must work on the FIRST Git
# call for the new snapshot generation; previously git apply rejected the
# zero-byte patch after creating the right checkout, making that first call fail.
EMPTY_REQ=$(python3 - "$MIRROR_CLIENT" "$MIRROR_HEAD" <<'PY'
import json, sys
print(json.dumps({"method": "workspace.mirror-sync", "args": [sys.argv[1]],
                  "transfer": "2123456789abcdef0123456789abcdef", "diff": "",
                  "head": sys.argv[2], "branch": "feature.locked",
                  "upstream": "origin/feature.locked"}))
PY
)
RESP=$(http_rpc "$EMPTY_REQ")
check_output "clean mirror-sync publishes a new generation" '"generation":2' echo "$RESP"

RESP=$(mcp_initialized_req "$GIT_REQ")
check_output "first MCP Git call accepts clean snapshot" 'feature.locked' echo "$RESP"
MIRROR_CLEAN_WORK=$(find "$(dirname "$SNAPSHOT")" -maxdepth 1 -type d -name 'work-2-*' \
    -print -quit)
check "clean snapshot worktree materialized on first call" test -n "$MIRROR_CLEAN_WORK"
check "clean snapshot worktree remains clean" test -z \
    "$(git -C "$MIRROR_CLEAN_WORK" status --porcelain)"

# ============================================================
# 10. Server shutdown
# ============================================================

kill "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

check "socket removed after shutdown" test ! -S "$HTTP_SOCK"

# ============================================================
# Results
# ============================================================

TOTAL=$((PASS + FAIL))
echo ""
echo "integration: $PASS/$TOTAL passed"
if [ "$SKIP" -gt 0 ]; then
    echo "SKIPPED ($SKIP knowledge-service checks; covered by full-stack E2E)"
fi
if [ "$FAIL" -gt 0 ]; then
    echo "FAILED ($FAIL failures)"
    exit 1
fi
echo "All integration tests passed."
