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
# Keep the real HOME's Go caches. HOME is about to become a temp dir, and a Go
# build under it would re-download the module graph on every run and then leave
# a read-only module cache behind that the teardown cannot remove.
INTEG_REAL_HOME="$HOME"
export GOCACHE="${GOCACHE:-$INTEG_REAL_HOME/.cache/go-build}"
export GOMODCACHE="${GOMODCACHE:-$INTEG_REAL_HOME/go/pkg/mod}"
export GOFLAGS="${GOFLAGS:--mod=mod}"
export HOME=$(mktemp -d /tmp/aimee-integ-XXXXXX)
export AIMEE_HOME="$HOME/.config/aimee"
unset AIMEE_PROFILE
# One session id for the whole run. Every `aimee mcp-serve` this harness spawns
# derives its session from AIMEE_SESSION_ID, falling back to its PPID -- and the
# helpers spawn each request from a separate python process, so without this
# every single MCP request minted a NEW session and materialized its own
# `git worktree add` of this entire repository. Eight full checkouts per run,
# where a real host has one session and one worktree.
#
# That is not just waste. The git tool call is the first request that has to
# reach aimee-server, and on a slow CI disk its checkout ran past the 30s
# mcp-serve request timeout, failing as "aimee-server unavailable after
# retries" while the checks either side of it reached that same server fine.
export AIMEE_SESSION_ID="integ$$"
mkdir -p "$AIMEE_HOME"
SOCKET="$AIMEE_HOME/aimee.sock"
export AIMEE_SOCK="$SOCKET"
HTTP_SOCK="$AIMEE_HOME/aimee-http.sock"
# The client reaches a server ONLY through an explicitly configured endpoint —
# it no longer finds a co-located one by probing the filesystem, because a stray
# local server quietly answering for the wrong host is worse than an outage.
# This harness deliberately runs both halves on one box, so it has to say so.
# Without this every client assertion below fails as "aimee-server unavailable"
# while the server it started is running perfectly well two lines away.
export AIMEE_API_ENDPOINT="unix:$HTTP_SOCK"


# ---------------------------------------------------------------
# The DB1 module.
#
# Most of DB1 is served by a separate process now, and the daemon has no
# in-process fallback: with nothing attached, every migrated call answers
# "capability absent" and the operation fails. In a container the module
# supervisor starts it from server.modules; this harness has no supervisor, so
# it does what the supervisor does -- install the module beside the socket,
# write the grant the daemon's policy loader reads, and attach it once the
# daemon is up.
#
# Without this the harness exercises a daemon that cannot reach its own store,
# and reports it as "failed to create session" rather than as a missing module.
#
# The grant is written only when the module is actually there. A grant naming an
# executable that does not resolve is not one the daemon ignores: it refuses to
# start the module bus, and the server exits before it listens. The make target
# builds the module, so the ordinary path has one; a hand-run without it
# degrades to the old behaviour rather than to a server that will not come up.
# ---------------------------------------------------------------
# One Go binary serves every module; which one it is comes from argv[0], so the
# harness stages it under the name the grant pins.
DB1_MODULE_BUILT="$REPO_ROOT/src/build/obj/aimee-module"
DB1_MODULE="$AIMEE_HOME/aimee-module-aimee"
# The same multicall binary under the postgres name: it derives its
# identity from argv[0], and the grant pins the resolved path.
PG_MODULE="$AIMEE_HOME/aimee-module-postgres"
PG_MODULE_PID=""
MODULE_POLICY_DIR="$AIMEE_HOME/modules.d/server"
MODULE_BUS_SOCK="$AIMEE_HOME/server-module-bus.sock"
DB1_MODULE_PID=""

# Configuration is served by its own pure-Go process. The daemon validates its
# startup snapshot before opening HTTP, so this module must attach as soon as
# the bus socket exists; waiting for HTTP first is a circular dependency.
CONFIG_MODULE_BUILT="$REPO_ROOT/src/build/obj/aimee-module-config"
CONFIG_MODULE="$AIMEE_HOME/aimee-module-config"
CONFIG_MODULE_PID=""

install_config_module() {
    if [ ! -x "$CONFIG_MODULE_BUILT" ]; then
        echo "ABORT: the config module is not built at $CONFIG_MODULE_BUILT."
        echo "       Run this harness through 'make integration-tests'."
        exit 1
    fi
    cp "$CONFIG_MODULE_BUILT" "$CONFIG_MODULE"
    chmod 0755 "$CONFIG_MODULE"
    mkdir -p "$MODULE_POLICY_DIR"
    local generated="$REPO_ROOT/src/build/obj/module-bundle/grants/server/config.grant"
    if [ ! -r "$generated" ]; then
        python3 "$REPO_ROOT/scripts/export_c_repositories.py" \
            --runtime-bundle "$REPO_ROOT/src/build/obj/module-bundle" >/dev/null 2>&1 || true
    fi
    if [ ! -r "$generated" ]; then
        echo "ABORT: no generated config module grant at $generated."
        exit 1
    fi
    sed "s|^executable=.*|executable=$CONFIG_MODULE|" "$generated" \
        >"$MODULE_POLICY_DIR/config.grant"
}

start_config_module() {
    [ -x "$CONFIG_MODULE" ] || return 1
    stop_config_module
    "$CONFIG_MODULE" "$MODULE_BUS_SOCK" >"$HOME/aimee-config.log" 2>&1 &
    CONFIG_MODULE_PID=$!
}

stop_config_module() {
    if [ -n "$CONFIG_MODULE_PID" ]; then
        kill "$CONFIG_MODULE_PID" 2>/dev/null || true
        wait "$CONFIG_MODULE_PID" 2>/dev/null || true
        CONFIG_MODULE_PID=""
    fi
}

install_db1_module() {
    # Missing module: stop, do not degrade. This used to return 0 and let the
    # run continue, from a time when the daemon still had an in-process store to
    # fall back to. It has none now, so a skipped install does not produce a
    # weaker run -- it produces a run where every store-backed check fails as
    # "failed to create session", which reads like a product bug and is not one.
    # The make target builds the module, so this only fires on a hand-run, which
    # is exactly the case that needs telling.
    if [ ! -x "$DB1_MODULE_BUILT" ]; then
        echo "ABORT: the DB1 module is not built at $DB1_MODULE_BUILT."
        echo "       Every store-backed check needs it: nothing serves the store"
        echo "       without it. Build it with:"
        echo "           make -C src build/obj/aimee-module"
        echo "       or run this harness through 'make integration-tests', which"
        echo "       builds it as a prerequisite."
        exit 1
    fi
    # Copied beside the socket rather than granted where it was built: a grant
    # pins a resolved path, and a build tree is not where a deployed module
    # lives.
    cp "$DB1_MODULE_BUILT" "$DB1_MODULE"
    chmod 0755 "$DB1_MODULE"
    cp "$DB1_MODULE_BUILT" "$PG_MODULE"
    chmod 0755 "$PG_MODULE"
    mkdir -p "$MODULE_POLICY_DIR"

    # Both extra grants come from the SAME generated bundle the store's own
    # grant does, rather than being written out here. The refs and kinds in
    # them are derived from src/modules/process-contracts.json, and a copy
    # transcribed into this file is a copy that goes stale silently: the
    # store's outbound ref moved from 68 to 69 in a merge, and a hand-written
    # heredoc would still have said 68 while every other site said 69.
    install_generated_grant() {
        local name="$1" exe="$2"
        local src="$REPO_ROOT/src/build/obj/module-bundle/grants/server/$name.grant"
        if [ ! -r "$src" ]; then
            python3 "$REPO_ROOT/scripts/export_c_repositories.py" \
                --runtime-bundle "$REPO_ROOT/src/build/obj/module-bundle" >/dev/null 2>&1 || true
        fi
        if [ ! -r "$src" ]; then
            echo "no generated $name grant at $src" >&2
            return 1
        fi
        sed "s|^executable=.*|executable=$exe|" "$src" \
            >"$MODULE_POLICY_DIR/$name.grant"
    }
    # The postgres module serves the SQL stage the store calls; the store's
    # OUTBOUND principal is what is allowed to call it. A serve grant admits
    # what a module answers, not what it asks for, so the second is not
    # implied by the first -- without it the store attaches and then finds no
    # backend, which reads exactly like a broken store.
    install_generated_grant postgres "$PG_MODULE" || exit 1
    install_generated_grant aimee-postgres "$DB1_MODULE" || exit 1

    # The serve list comes from the grant the exporter generates, so it cannot
    # drift from what the module actually serves. It is a build artifact, so
    # generate it when it is not there rather than guessing: the guess this
    # replaced was a hardcoded list of eight kinds, written when the module
    # served eight families. It serves nineteen. A short list does not fail
    # loudly -- the daemon starts, eleven families are simply unserved, and
    # their checks fail as if the code were broken.
    local generated="$REPO_ROOT/src/build/obj/module-bundle/grants/server/aimee.grant"
    if [ ! -r "$generated" ]; then
        python3 "$REPO_ROOT/scripts/export_c_repositories.py" \
            --runtime-bundle "$REPO_ROOT/src/build/obj/module-bundle" >/dev/null 2>&1 || true
    fi
    local serve="" ref=""
    if [ -r "$generated" ]; then
        serve=$(sed -n 's/^serve=//p' "$generated" || true)
        # From the file for the same reason the serve list is: a ref restated
        # here can drift from the one the module registers under, and the
        # failure that produces is a module nobody can reach.
        ref=$(sed -n 's/^principal_ref=//p' "$generated" || true)
    fi
    if [ -z "$serve" ]; then
        echo "ABORT: no generated store grant at $generated, and it could not be"
        echo "       generated. Without it there is no honest serve list to"
        echo "       install: run"
        echo "           python3 scripts/export_c_repositories.py \\"
        echo "               --runtime-bundle src/build/obj/module-bundle"
        exit 1
    fi
    cat >"$MODULE_POLICY_DIR/aimee.grant" <<GRANT
version=1
principal_class=1
principal_ref=${ref:-30}
uid=self
executable=$DB1_MODULE
publish=
subscribe=
request=
serve=$serve
GRANT
}

start_db1_module() {
    [ -x "$DB1_MODULE" ] || return 0
    # The store is a Go module against PostgreSQL and reads AIMEE_STORE_URL; it
    # opened a SQLite file named by AIMEE_DB1_PATH until it moved. Skip the same
    # way an unbuilt module is skipped, but SAY so -- starting it without a DSN
    # would fork a process that exits immediately and leave the wait below to
    # burn ten seconds discovering a socket that is never coming.
    if [ -z "${AIMEE_STORE_URL:-}" ]; then
        echo "integration: AIMEE_STORE_URL unset; skipping the store module" >&2
        return 0
    fi
    if [ -z "${AIMEE_STORE_MIGRATION_URL:-}" ]; then
        echo "integration: AIMEE_STORE_MIGRATION_URL unset; refusing an unprivileged or implicit migration" >&2
        return 1
    fi
    # One schema per run. Without it the run inherits every row the last one
    # wrote -- the harness used to get a fresh SQLite file each time and the
    # isolation came free. The module creates the schema it is pointed at, so
    # this needs no CREATE DATABASE right and no postgres client here.
    if [ -z "${AIMEE_STORE_SCHEMA:-}" ]; then
        AIMEE_STORE_SCHEMA="integ_$$"
        case "$AIMEE_STORE_URL" in
            *\?*) AIMEE_STORE_URL="$AIMEE_STORE_URL&search_path=$AIMEE_STORE_SCHEMA" ;;
            *)    AIMEE_STORE_URL="$AIMEE_STORE_URL?search_path=$AIMEE_STORE_SCHEMA" ;;
        esac
        case "$AIMEE_STORE_MIGRATION_URL" in
            *\?*) AIMEE_STORE_MIGRATION_URL="$AIMEE_STORE_MIGRATION_URL&search_path=$AIMEE_STORE_SCHEMA" ;;
            *)    AIMEE_STORE_MIGRATION_URL="$AIMEE_STORE_MIGRATION_URL?search_path=$AIMEE_STORE_SCHEMA" ;;
        esac
        export AIMEE_STORE_URL AIMEE_STORE_MIGRATION_URL AIMEE_STORE_SCHEMA
    fi
    stop_db1_module
    # Postgres first: the store looks for its backend as it comes up, so a store
    # started into an empty bus fails immediately rather than waiting.
    if [ -x "$PG_MODULE" ] && [ -z "$PG_MODULE_PID" ]; then
        AIMEE_STORE_URL="$AIMEE_STORE_URL" \
            AIMEE_STORE_MIGRATION_URL="$AIMEE_STORE_MIGRATION_URL" \
            "$PG_MODULE" "$MODULE_BUS_SOCK" \
            >"$AIMEE_HOME/pg-module.log" 2>&1 &
        PG_MODULE_PID=$!
    fi
    # Its output goes to a file, not /dev/null: a module that refuses to start
    # says why exactly once, and discarding that leaves the failure looking like
    # a socket that never appeared.
    AIMEE_STORE_URL="$AIMEE_STORE_URL" \
        AIMEE_STORE_MIGRATION_URL="$AIMEE_STORE_MIGRATION_URL" \
        "$DB1_MODULE" "$MODULE_BUS_SOCK" >"$AIMEE_HOME/db1-module.log" 2>&1 &
    DB1_MODULE_PID=$!
    # Wait for the store to be SERVING, not for the bus socket: the daemon owns
    # that socket and creates it before the module is forked, so polling it fell
    # through immediately and the harness ran on against a store that had not
    # attached. The module now also opens a connection pool and applies its
    # schema before it serves, which made that window wide enough to fail a
    # health assertion.
    #
    # The daemon's own health state is the condition every caller depends on, so
    # that is what this waits for.
    local i
    for i in $(seq 1 200); do
        if [ -S "$HTTP_SOCK" ] && curl -s --max-time 2 --unix-socket "$HTTP_SOCK" \
                http://localhost/v1/server/health 2>/dev/null | grep -q '"state":"ok"'; then
            return 0
        fi
        if ! kill -0 "$DB1_MODULE_PID" 2>/dev/null; then
            echo "integration: the store module exited before it served:" >&2
            sed 's/^/    /' "$AIMEE_HOME/db1-module.log" 2>/dev/null | head -5 >&2
            return 1
        fi
        sleep 0.1
    done
    echo "integration: the store module never reached serving; its log was:" >&2
    sed 's/^/    /' "$AIMEE_HOME/db1-module.log" 2>/dev/null | head -5 >&2
    return 1
}

stop_db1_module() {
    if [ -n "$DB1_MODULE_PID" ]; then
        kill "$DB1_MODULE_PID" 2>/dev/null || true
        wait "$DB1_MODULE_PID" 2>/dev/null || true
        DB1_MODULE_PID=""
    fi
}


# ---------------------------------------------------------------
# The workflow control module. Every /v1/workflow route and /v1/dev/submit is
# dispatched to it over the bus -- the daemon holds no workflow logic and
# answers 503 when nothing is attached -- so without this the entire workflow
# surface is untested, which is exactly how it went untested until now.
#
# It is the Go binary (deployed as aimee-wfe) and it needs TWO grants for the
# same executable: one serving identity for the control kinds it answers, and a
# separate outbound identity for the store kinds it calls. A module's serving
# grant requests nothing, so a single grant cannot do both.
# ---------------------------------------------------------------
WFE_MODULE_SRC="$REPO_ROOT/server-go"
WFE_MODULE_BUILT="$REPO_ROOT/src/build/obj/aimee-wfe"
WFE_MODULE="$AIMEE_HOME/aimee-wfe"
WFE_MODULE_PID=""
WORKFLOW_MODULE_READY=0

install_workflow_module() {
    [ -d "$WFE_MODULE_SRC" ] || return 1
    command -v go >/dev/null 2>&1 || return 1
    ( cd "$WFE_MODULE_SRC" && go build -buildvcs=false -o "$WFE_MODULE_BUILT" ./cmd/aimee-server ) \
        >/dev/null 2>&1 || return 1
    cp "$WFE_MODULE_BUILT" "$WFE_MODULE"
    chmod 0755 "$WFE_MODULE"
    # The definitions a deployment ships. Without them the engine starts with an
    # empty registry, and every submit fails to resolve its workflow -- which
    # looks like a broken intake rather than an empty install.
    mkdir -p "$AIMEE_HOME/workflows"
    cp "$REPO_ROOT"/config/workflows/*.yaml "$AIMEE_HOME/workflows/" 2>/dev/null || return 1
    mkdir -p "$MODULE_POLICY_DIR"
    # Same rule as the DB1 grant: take the generated one so the serve/request
    # lists cannot drift from what the module actually serves, and rewrite only
    # the executable, because a grant pins a resolved path and this one is
    # installed beside the socket rather than at its deployed location.
    local bundle="$REPO_ROOT/src/build/obj/module-bundle/grants/server"
    local g
    for g in wfe workflows; do
        [ -r "$bundle/$g.grant" ] || return 1
        sed "s|^executable=.*|executable=$WFE_MODULE|" "$bundle/$g.grant" \
            >"$MODULE_POLICY_DIR/$g.grant"
    done
    return 0
}

start_workflow_module() {
    [ -x "$WFE_MODULE" ] || return 1
    stop_workflow_module
    "$WFE_MODULE" --home "$AIMEE_HOME" --socket "$AIMEE_HOME/aimee-wfe.sock" \
        --module-bus-socket "$MODULE_BUS_SOCK" >"$HOME/aimee-wfe.log" 2>&1 &
    WFE_MODULE_PID=$!
    # Attachment is what matters, not the process: poll the seam itself until it
    # stops answering "not attached". A fixed sleep here would be a flake.
    local i
    for i in $(seq 1 100); do
        [ "$(http_status GET /v1/workflow/defs 2>/dev/null)" = "200" ] && return 0
        kill -0 "$WFE_MODULE_PID" 2>/dev/null || break
        sleep 0.1
    done
    return 1
}

stop_workflow_module() {
    if [ -n "$WFE_MODULE_PID" ]; then
        kill "$WFE_MODULE_PID" 2>/dev/null || true
        wait "$WFE_MODULE_PID" 2>/dev/null || true
        WFE_MODULE_PID=""
    fi
}

install_db1_module
install_config_module
# Grants are read by the daemon at startup, so this has to happen BEFORE the
# server is started even though the module itself is not launched until the
# workflow section. Installing it later produced a module that ran, attached to
# nothing, and left the whole workflow surface answering 503.
WORKFLOW_MODULE_INSTALLED=0
if install_workflow_module; then
    WORKFLOW_MODULE_INSTALLED=1
fi

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
        dump_server_log
        FAIL=$((FAIL + 1))
    fi
}

# What the server was doing when a check failed. Bounded, and only for the
# first few failures: enough to diagnose, not enough to bury the summary.
# Without this a server-side stall is invisible -- the client reports only that
# it gave up, which reads as "the server is down" even while the checks either
# side of it are served fine.
SERVER_LOG=""
SERVER_LOG_DUMPS=0
dump_server_log() {
    [ "$SERVER_LOG_DUMPS" -lt 3 ] || return 0
    SERVER_LOG_DUMPS=$((SERVER_LOG_DUMPS + 1))
    # Say which case this is rather than going quiet. A silent helper here would
    # repeat, in miniature, the bug it exists to fix: the reader cannot tell
    # "the server said nothing" from "nobody looked".
    # Two different files, and the useful one is not the obvious one. The
    # redirect on start_server captures only what the server writes to
    # stdout/stderr, which for a healthy boot is nothing; the request log --
    # every route, its status and its timing -- goes to AIMEE_HOME/server.log.
    # Prefer that, and fall back to the capture so a server that dies before it
    # can open its log still gets to say why.
    local shown=0
    local f
    for f in "$AIMEE_HOME/server.log" "$SERVER_LOG"; do
        [ -n "$f" ] && [ -s "$f" ] || continue
        echo "  aimee-server log (last 20 lines of $f):"
        tail -20 "$f" 2>/dev/null | sed 's/^/    /'
        shown=1
        break
    done
    [ "$shown" -eq 1 ] || echo "  aimee-server log: nothing recorded in $AIMEE_HOME/server.log or ${SERVER_LOG:-(no capture path)}"
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
        dump_server_log
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
# Speak a verb and a path directly, and report the STATUS as well as the body.
# The workflow control plane answers with distinct codes for distinct outcomes
# -- 429 for a capped principal, 503 for a store that could not answer -- and a
# helper that returned only the body could not tell those apart, which is the
# one distinction that section is there to check.
http_call() {
    python3 -c "
import socket, sys
verb, path, body = sys.argv[1], sys.argv[2], sys.argv[3]
req = ('%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n'
       'Content-Length: %d\r\nConnection: close\r\n\r\n%s' % (verb, path, len(body), body))
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$HTTP_SOCK')
s.settimeout(20)
s.sendall(req.encode())
data = b''
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
s.close()
head, _, payload = data.partition(b'\r\n\r\n')
status = head.split(b'\r\n')[0].split(b' ')[1].decode() if head else '000'
sys.stdout.write(status + ' ' + payload.decode(errors='replace').strip())
" "$1" "$2" "${3:-}"
}

# The status alone, for checks that care only about the outcome code.
http_status() {
    http_call "$1" "$2" "${3:-}" | cut -d' ' -f1
}

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
err = p.stderr.read().decode("utf-8", "replace").strip()
if err:
    sys.stderr.write("mcp-serve stderr:\n" + err + "\n")
PY
}

# mcp-serve runs from MCP_CWD, never from the source tree it was built in.
#
# A `git` tool call makes the client ship its CWD's working-tree diff to the
# server. Run from src/, that diff is whatever the developer happens to have
# uncommitted -- so this suite's result depended on the state of the checkout it
# was invoked from, and six mirror checks failed for anyone carrying more than a
# few megabytes of unpushed work. The tool calls below all pass absolute paths,
# so the cwd is not otherwise load-bearing.
MCP_CWD="$AIMEE_HOME/mcp-cwd"
mkdir -p "$MCP_CWD"

mcp_initialized_req() {
    python3 - "$AIMEE" "$1" "$MCP_CWD" <<'PY'
import subprocess
import sys

cmd = [sys.argv[1], "mcp-serve"]
request = sys.argv[2]
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     cwd=sys.argv[3])

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
err = p.stderr.read().decode("utf-8", "replace").strip()
if err:
    sys.stderr.write("mcp-serve stderr:\n" + err + "\n")
PY
}

# Start aimee-server and WAIT for it to bind, rather than guessing.
#
# This was `... >/dev/null 2>&1 & sleep 1`, three times. Both halves were wrong,
# and test_init_migrate_service.sh had already found and fixed exactly this:
# "On a loaded CI runner startup regularly takes several seconds, and the 1s
# guess made this the run's flakiest test." It was left unfixed here, and the
# first CI run of this harness failed 14 checks off one early probe -- the
# server had not finished binding, so "server started" failed and every request
# after it cascaded.
#
# The discarded output was the other half. A server that fails to start says why
# on stderr; sending that to /dev/null left CI reporting "FAIL: server started"
# and nothing else, which is unactionable. Keep the log and print its tail when
# the socket never appears.
#
# A unix-socket server binds and listens as its last init step, so
# socket-present means ready to serve. Polling also returns as soon as it is up,
# which is faster than the sleep it replaces on a machine that is not loaded.
start_server() {
    # Global, not local: a failing CHECK needs this as much as a failing bind.
    # A server that answers slowly, or not at all, says why here and nowhere
    # else, and every check that reads only the client's side has to guess.
    SERVER_LOG="$HOME/aimee-server.$$.log"
    local log="$SERVER_LOG"
    "$AIMEE_SERVER" --foreground >"$log" 2>&1 &
    SERVER_PID=$!
    local config_started=0
    local i
    for i in $(seq 1 300); do
        if [ "$config_started" -eq 0 ] && [ -S "$MODULE_BUS_SOCK" ]; then
            start_config_module
            config_started=1
        fi
        [ -S "$HTTP_SOCK" ] && { start_db1_module; return 0; }
        # A dead process will never bind; fail fast rather than waiting out the
        # full window for a server that has already exited.
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.1
    done
    echo "server did not bind $HTTP_SOCK within 30s; its own output was:"
    sed 's/^/    /' "$log" 2>/dev/null | tail -20
    return 1
}

# Set once the summary has printed. Until then an exit is an ABORT, not a
# result: `set -e` means any unguarded command that fails takes the whole run
# with it, skipping every remaining check AND the summary. In CI that looked
# like a job which built for four minutes, ran the harness, and printed nothing
# at all — no FAIL line, no count, nothing to read. Say so instead.
REACHED_SUMMARY=0
ABORT_LINE=""
ABORT_CMD=""
# Record WHERE, not just that. "ABORTED after 21 checks" is a number to go
# hunting with; bash already knows the line and the command, so ask it.
trap 'ABORT_LINE=$LINENO; ABORT_CMD=$BASH_COMMAND' ERR

stop_pg_module() {
    if [ -n "$PG_MODULE_PID" ]; then
        kill "$PG_MODULE_PID" 2>/dev/null || true
        wait "$PG_MODULE_PID" 2>/dev/null || true
        PG_MODULE_PID=""
    fi
}

cleanup() {
    stop_workflow_module
    stop_db1_module
    stop_pg_module
    stop_config_module
    local rc=$?
    if [ "$REACHED_SUMMARY" -ne 1 ]; then
        echo ""
        echo "integration: ABORTED after $((PASS + FAIL)) checks (exit $rc)."
        echo "  The harness exited before its summary — under 'set -e' an unguarded"
        echo "  command failed, so the checks below this point never ran."
        if [ -n "$ABORT_LINE" ]; then
            echo "  It failed at line $ABORT_LINE: $ABORT_CMD"
        fi
    fi
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    # Same rule as the TCP teardown: removing a temp dir must never change the
    # verdict. This one already waits for its server, which is why it never bit.
    rm -rf "$HOME" || true
}
trap cleanup EXIT

# ============================================================
# Setup: server initializes DB1 on startup
# ============================================================

# ============================================================
# 1. No server reachable
# ============================================================
# This used to be "auto-start", and asserted that a command failed *because
# autostart was disabled*. There is no autostart now: the client starts no
# server, ever, so an unreachable server is simply an outage.

check_output "no server: version works" "aimee" $AIMEE version

# A pure client runs no command handler locally, so this must fail rather than
# answer from local state. It now fails one step earlier than it used to -- the
# client cannot even learn that `memory list` exists without the server -- which
# is the point: the command table lives server-side.
check_output "no server: a command fails, naming the unreachable server" \
    "cannot reach aimee-server" $AIMEE memory list

# ============================================================
# 2. Server lifecycle
# ============================================================

# NOT `check "server started" start_server`: check() runs its command with
# >/dev/null 2>&1, which would swallow the very diagnosis start_server exists to
# print. Run it first, then assert on the result. (Found by planting a server
# that refuses to start: the 14 cascading failures reproduced perfectly and the
# reason was still invisible.)
start_server && SERVER_STARTED=1 || SERVER_STARTED=0
check "server started" test "$SERVER_STARTED" = 1

RESP=$(srv_req '{"method":"server.info"}') || true
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

RESP=$(srv_req '{"method":"server.health"}') || true
check_output "server.health status" '"status":"ok"' echo "$RESP"
check_output "server.health uptime" '"uptime"' echo "$RESP"
# The DB1 state is the server's answer to "can I reach the store", and since
# the store became a module that is a question about the module being
# attached rather than about a file this process opened. When this run has a
# PostgreSQL DSN the module must be reachable. The documented no-DSN path above
# deliberately skips it, so health must honestly report that degraded state
# instead of pretending the store is available.
if [ -n "$DB1_MODULE_PID" ]; then
    check_output "server.health reports the DB1 store reachable" '"state":"ok"' echo "$RESP"
else
    check_output "server.health reports the skipped DB1 store unavailable" '"state":"unavailable"' echo "$RESP"
fi

# ... and stops reporting it once the store is gone, promptly.
#
# "Promptly" is the whole point. This used to be read from module availability,
# which is registry state the bus corrects on a 30s heartbeat with a reap every
# 7.5s -- so health kept answering "ok" for ~37s after the module died while
# every store call was already failing. Measured twice on a clean container at
# 36.5s and 37s before the fix, 1s after it.
#
# Five seconds is the budget here: the probe caches for one, and the rest is
# slack for a loaded CI box. A regression to inferring availability rather than
# probing it fails this by twenty seconds, not by a hair.
if [ -n "$DB1_MODULE_PID" ]; then
    stop_db1_module
    HEALTH_GONE=""
    for _i in $(seq 1 25); do
        HEALTH_GONE=$(http_rpc '{"method":"server.health"}') || true
        case "$HEALTH_GONE" in
        *'"state":"ok"'*) sleep 0.2 ;;
        *) break ;;
        esac
    done
    case "$HEALTH_GONE" in
    *'"state":"ok"'*)
        echo "FAIL: server.health still reported the store ok 5s after the module was killed"
        echo "  health: $HEALTH_GONE"
        FAIL=$((FAIL + 1))
        ;;
    *)
        PASS=$((PASS + 1)) # server.health notices the store is gone
        ;;
    esac
    # Put it back: every check after this one needs a store.
    start_db1_module
    for _i in $(seq 1 50); do
        case "$(http_rpc '{"method":"server.health"}' || true)" in
        *'"state":"ok"'*) break ;;
        *) sleep 0.2 ;;
        esac
    done
fi

# index.investigate refuses rather than guessing when no project is in scope:
# an unscoped investigation would silently search the wrong repository.
RESP=$(srv_req '{"method":"index.investigate","query":"where is the shared date helper"}') || true
check_output "index.investigate refuses without a project scope" 'scope_required' echo "$RESP"

# With a scope, it must distinguish an OUTAGE from an index with no evidence.
# The integration server runs without a reachable knowledge service, so this is
# exactly the condition that used to answer "no evidence" and send the agent off
# to search the tree by hand. It is the call the session guidance tells every
# agent to make FIRST, so a silent outage costs the whole opening move.
RESP=$(srv_req '{"method":"index.investigate","query":"where is the shared date helper","project":"integration-scope"}') || true
check_output "index.investigate names the dependency that failed" '"dependency":"kb"' echo "$RESP"
check_output "index.investigate reports an outage, not empty evidence" \
    'not an index with no evidence' echo "$RESP"
check_output "index.investigate marks the outage retryable" '"retryable":true' echo "$RESP"

# The /v1 HTTP surface is the only transport now (the NDJSON RPC socket was
# removed). Confirm the local /v1 UDS is bound and an allowlisted read returns a
# well-formed dispatch response over its first-class /v1 route.
if [ -S "$HTTP_SOCK" ]; then
    PASS=$((PASS + 1)) # /v1 HTTP socket bound
    HTTP_PROV=$(http_rpc '{"method":"provider.list"}') || true
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
TCP_HOME=$(mktemp -d /tmp/aimee-tcp-XXXXXX) || true
mkdir -p "$TCP_HOME/.config/aimee/modules.d/server"
TCP_PORT=18897
TCP_BEARER="integ-tcp-bearer"
printf 'aimee:\n  api:\n    http_port: %s\n' \
    "$TCP_PORT" >"$TCP_HOME/.config/aimee/aimee.yaml"
sed "s|^executable=.*|executable=$CONFIG_MODULE_BUILT|" \
    "$REPO_ROOT/src/build/obj/module-bundle/grants/server/config.grant" \
    >"$TCP_HOME/.config/aimee/modules.d/server/config.grant"
env -u AIMEE_PROFILE HOME="$TCP_HOME" AIMEE_HOME="$TCP_HOME/.config/aimee" \
    AIMEE_API_BEARER_TOKEN="$TCP_BEARER" \
    "$AIMEE_SERVER" --foreground >"$TCP_HOME/server.log" 2>&1 &
TCP_SRV_PID=$!
TCP_BUS_SOCK="$TCP_HOME/.config/aimee/server-module-bus.sock"
for _ in $(seq 1 300); do
    [ -S "$TCP_BUS_SOCK" ] && break
    kill -0 "$TCP_SRV_PID" 2>/dev/null || break
    sleep 0.1
done
env HOME="$TCP_HOME" AIMEE_HOME="$TCP_HOME/.config/aimee" \
    "$CONFIG_MODULE_BUILT" "$TCP_BUS_SOCK" >"$TCP_HOME/config.log" 2>&1 &
TCP_CONFIG_PID=$!
# Wait for the bind rather than guessing at it, for the same reason as
# start_server above: on a loaded runner two seconds is not reliably enough, and
# a probe that fires early fails the assertions of a server that was about to
# work. This one has its own HOME so it cannot share start_server.
for _ in $(seq 1 300); do
    [ -S "$TCP_HOME/.config/aimee/aimee-http.sock" ] && break
    kill -0 "$TCP_SRV_PID" 2>/dev/null || break
    sleep 0.1
done
UDS_BODY=$(HTTP_SOCK="$TCP_HOME/.config/aimee/aimee-http.sock" http_rpc '{"method":"provider.list"}') || true
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
# Kill the TCP server and WAIT for it, before removing the home it is writing
# to. Without the wait, `rm -rf` raced a live server recreating files as the
# walk deleted them, and failed with

#     rm: cannot remove '/tmp/aimee-tcp-XXXXXX': Directory not empty

# which under `set -e` took the entire run with it -- every check after this
# point, on every CI run of this harness. That line was in the log from the
# first failing run and read as noise; it was the cause.
kill "$TCP_SRV_PID" 2>/dev/null || true
kill "$TCP_CONFIG_PID" 2>/dev/null || true
# Reap before removing its HOME. kill only requests exit, so without this the
# server can still be writing under $TCP_HOME while rm -rf walks it, and rm
# fails with ENOTEMPTY when a directory gains entries between unlinking its
# children and removing it. Under 'set -e' that aborts the whole harness.
wait "$TCP_SRV_PID" 2>/dev/null || true
wait "$TCP_CONFIG_PID" 2>/dev/null || true
# `|| true` regardless: teardown of a temporary directory must never decide
# whether the suite continues. Even with the wait, anything else holding a file
# open here would abort a run that has nothing left to do but report.
rm -rf "$TCP_HOME" || true

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
    start_server || true
fi

check_output "local version long flag" "aimee" $AIMEE --version
check "index overview route" $AIMEE index overview

RESP=$(mcp_framed_req '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"integration-test","version":"1"}}}') || true
check_output "mcp initialize over stdio framing" '"protocolVersion":"2024-11-05"' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":2,"method":"prompts/get","params":{"name":"search-and-summarize","arguments":{"query":"mcp"}}}') || true
check_output "mcp prompts/get" '"messages"' echo "$RESP"
check_output "mcp prompts/get text" 'search_memory' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":3,"method":"resources/templates/list","params":{}}') || true
check_output "mcp resources/templates/list" '"resourceTemplates"' echo "$RESP"
check_output "mcp resources/templates/list tier template" 'aimee://memories/{tier}' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"aimee://config"}}') || true
check_output "mcp resources/read config" '"mimeType":"application/json"' echo "$RESP"
check_output "mcp resources/read config text" '\"protocolVersion\":\"2024-11-05\"' echo "$RESP"

# Retry ONCE when the bridge reports the server unreachable.
#
# A tools/call is a POST, and cli_mcp_serve.c deliberately never retries those:
# "a lost response must not duplicate a mutation whose first attempt may have
# completed." That is right, and it makes this check single-shot -- one slow
# response on a loaded runner fails it. It has failed twice now for two
# different reasons, which makes it the flakiest check here.
#
# So retry the CHECK, not the request. Safe precisely because git_status is a
# read: a second attempt cannot duplicate anything, which is the whole reason
# the product refuses to retry the general case. Anything other than "server
# unavailable" is a real answer and is asserted on as-is, so a genuine
# regression still fails on the first attempt.
mcp_git_status() {
    local out
    out=$(mcp_initialized_req '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"git_status","arguments":{}}}' 2>/dev/null) || true
    case "$out" in
        *"unavailable after retries"*)
            echo "  (mcp git_status: bridge reported the server unreachable; retrying once)" >&2
            sleep 2
            out=$(mcp_initialized_req '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"git_status","arguments":{}}}' 2>/dev/null) || true
            ;;
    esac
    printf '%s' "$out"
}

RESP=$(mcp_git_status) || true
check_output "mcp git_status tool call" '"content"' echo "$RESP"
check_output "mcp git_status result text" 'branch:' echo "$RESP"

RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"get_help","arguments":{}}}') || true
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
RESP=$(srv_req '{"method":"memory.list","limit":1}') || true
if echo "$RESP" | grep -qF '"status":"ok"'; then
    KB_AVAILABLE=1
    PASS=$((PASS + 1))
    RESP=$(srv_req '{"method":"rules.list"}') || true
    check_output "local /v1: rules.list ok" '"status":"ok"' echo "$RESP"
else
    KB_AVAILABLE=0
    echo "SKIP: local /v1 memory/rules reads (aimee-kb is not configured)"
    SKIP=$((SKIP + 2))
fi

# A write/control method is reachable over the trusted local socket — it must NOT
# come back capability-denied ("not permitted over /v1").
RESP=$(srv_req '{"method":"tool.execute","tool":"bash","arguments":"{\"command\":\"echo hi\"}","session_id":"t","cwd":"/tmp","timeout_ms":1000}') || true
if echo "$RESP" | grep -q "not permitted over /v1"; then
    echo "FAIL: local /v1: tool.execute unexpectedly capability-gated"
    echo "  resp: $RESP"
    FAIL=$((FAIL + 1))
else
    PASS=$((PASS + 1)) # tool.execute reachable over the trusted local /v1 socket
fi

# The argument specs, as the REAL server actually serves them.
#
# Three things were proven separately and never joined up: the differential test
# reads the data file directly, the end-to-end proof drives a hand-written stub
# manifest, and nothing checked that the server's emitter puts those specs into
# the manifest at all. cli_argspec_defs_to_json() drops a row whose spec fails
# to parse --
#
#     cJSON *spec = cJSON_Parse(d->spec);
#     if (!spec)
#        continue;
#
# -- so a typo in one spec string would silently stop that method being served,
# the client would fall back to its compiled marshaller, and every existing test
# would still pass. Ask the running server what it serves.
# GET, directly. Not via http_rpc: that maps an unmapped method to a POST, and
# the manifest route is GET-only -- so the first version of this check POSTed,
# got a non-empty ERROR body, and its "if the answer was empty, try a GET"
# fallback never fired. It then parsed the error as a manifest and reported
# zero specs, blaming the server for the check's own bug.
MANIFEST=$(python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$HTTP_SOCK')
s.settimeout(20)
s.sendall(b'GET /v1/cli/manifest HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
data = b''
while True:
    c = s.recv(65536)
    if not c:
        break
    data += c
s.close()
sys.stdout.write(data.partition(b'\r\n\r\n')[2].decode().strip())
" 2>/dev/null) || true

# Every served body, not just the one that prompted this.
#
# The manifest carries four: routes, commands, dispatch, marshal. A broken
# emitter for any of the last three makes the client fall back to its compiled
# copy SILENTLY -- the same failure the marshal check below exists for. (routes
# is the exception: losing it fails loudly with "has no /v1 route".)
#
# Checked by SHAPE, not by naming commands: a count floor catches an emitter
# that returns nothing, and the expected keys on one row catch a rename that
# would keep the count and break every client. Neither breaks when a command is
# added or retired, which a "contains init.run" assertion would.
MANIFEST_BODIES=$(printf '%s' "$MANIFEST" | python3 -c "
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    print('unparseable'); raise SystemExit
want = {
    'routes':   {'op', 'verb', 'path'},
    'commands': {'name'},
    'dispatch': {'cmd', 'method'},
    'marshal':  {'method', 'args'},
}
bad = []
for key, keys in want.items():
    rows = doc.get(key)
    if not isinstance(rows, list) or len(rows) < 10:
        bad.append(key + ':empty')
        continue
    if not any(keys <= set(r) for r in rows if isinstance(r, dict)):
        bad.append(key + ':shape')
print(','.join(bad) if bad else 'ok')
" 2>/dev/null) || true
check_output "every served manifest body is present and shaped" "ok" echo "$MANIFEST_BODIES"

MANIFEST_SPECS=$(printf '%s' "$MANIFEST" | python3 -c "
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    print('0'); raise SystemExit
rows = doc.get('marshal') or []
# An argument spec is a marshal row whose args is an OBJECT; the no-argument
# rows carry the string 'none'.
print(sum(1 for r in rows if isinstance(r.get('args'), dict)))
" 2>/dev/null) || true
check_output "server serves argument specs in the manifest" "yes" \
    echo "$([ "${MANIFEST_SPECS:-0}" -gt 0 ] && echo yes || echo "no (got ${MANIFEST_SPECS:-none})")"

# And that a known spec arrives INTACT -- not merely that some rows exist. A
# renamed key in the emitter would keep the count and break every client.
CATALOG_SPEC=$(printf '%s' "$MANIFEST" | python3 -c "
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    raise SystemExit
for r in (doc.get('marshal') or []):
    if r.get('method') == 'catalog.list' and isinstance(r.get('args'), dict):
        names = [f.get('json') for f in (r['args'].get('fields') or [])]
        print(','.join(sorted(n for n in names if n)))
        break
" 2>/dev/null) || true
check_output "a served spec arrives with its fields" "capability,json,open_weights_only" \
    echo "$CATALOG_SPEC"

# ============================================================
# 4. Session management
# ============================================================

# Sessions are persisted through the DB1 sessions stage, which the db1/db2
# event-bus conversion has moved OUT of this process: db1_client/sessions.c
# calls obs_bus_module_available() first and answers "failed to create session"
# with nothing serving the stage. The server does not launch modules; a
# container's module-supervisor does, and start_db1_module above now does the
# same thing here, so the stage IS reachable and these run as ordinary checks.
#
# The probe stays. It was written when the stage was absent by construction, and
# it is still the right shape: it is the switch rather than a hardcoded
# expectation, so a run without a built module degrades to skips instead of to
# a wall of failures. What changed is which way it answers.
RESP=$(srv_auth_req '{"method":"session.create","client_type":"test"}') || true
if echo "$RESP" | grep -qF '"status":"ok"'; then
    DB1_SESSIONS_AVAILABLE=1
    PASS=$((PASS + 1))
else
    DB1_SESSIONS_AVAILABLE=0
    echo "SKIP: session management (the DB1 sessions stage is not reachable from this process)"
    SKIP=$((SKIP + 12))
fi

if [ "$DB1_SESSIONS_AVAILABLE" -eq 1 ]; then
# `|| true`: under `set -e` a failed extraction aborts the entire run at the
# assignment, which is how one broken response silently swallowed every check
# after it. An empty SID fails its own assertions instead, visibly.
SID=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['session_id'])" 2>/dev/null || true)

RESP=$(srv_auth_req '{"method":"session.list"}') || true
check_output "session.list has session" "$SID" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.get\",\"session_id\":\"$SID\"}") || true
check_output "session.get" '"client_type":"test"' echo "$RESP"

RESP=$($AIMEE session list 2>&1) || true
check_output "client session list via server" "$SID" echo "$RESP"

RESP=$($AIMEE session show "$SID" 2>&1) || true
check_output "client session show via server" "client:      test" echo "$RESP"

# Not --limit 1: that asserts this session is the NEWEST, and an mcp session
# created in the same second wins the tie. The claim being tested is that the
# session is listed and the output is JSON, neither of which is about recency.
RESP=$($AIMEE --json session list --limit 20 2>&1) || true
check_output "client session list json" "$SID" echo "$RESP"

RESP=$(srv_auth_req '{"method":"session.create","client_type":"test-close"}') || true
check_output "session.create for client close" '"status":"ok"' echo "$RESP"
CLOSE_SID=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['session_id'])" 2>/dev/null || true)

RESP=$($AIMEE session close "$CLOSE_SID" 2>&1) || true
check_output "client session close via server" "$CLOSE_SID" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.get\",\"session_id\":\"$CLOSE_SID\"}") || true
check_output "client session close removed session" "session not found" echo "$RESP"

RESP=$(srv_auth_req "{\"method\":\"session.close\",\"session_id\":\"$SID\"}") || true
check_output "session.close" '"status":"ok"' echo "$RESP"

RESP=$(srv_auth_req '{"method":"session.list"}') || true
# The sessions this block opened are gone -- not "the list is empty". The MCP
# checks above open a session of their own and never close it, so an empty list
# is only true when sessions do not actually persist. That was the case while
# nothing served the DB1 sessions stage and this whole block was skipped; with
# the module attached the leftover is real, and asserting emptiness tested the
# absence of persistence rather than the behaviour of close.
check "session.list drops the closed session" \
    sh -c "! echo '$RESP' | grep -q \"$SID\""
check "session.list drops the client-closed session" \
    sh -c "! echo '$RESP' | grep -q \"$CLOSE_SID\""
fi  # DB1_SESSIONS_AVAILABLE

# ============================================================
# 5. Memory via server
# ============================================================

# These argument failures are typed INVALID_ARGUMENT replies. The dedicated
# HTTP adapter must preserve that mapping as a physical 400 instead of masking
# a rejected write behind 200 OK.
RESP=$(http_call POST /v1/memory/store '{"key":"integ-invalid"}') || true
check_output "memory.store missing content stays typed" '"kind":"invalid_argument"' echo "$RESP"
if echo "$RESP" | grep -q '"http_status":400'; then
    check_output "memory.store missing content returns HTTP 400" "400 " echo "$RESP"
else
    echo "SKIP: memory.store missing-content HTTP mapping (runtime-web status provider is not attached)"
    SKIP=$((SKIP + 1))
fi

RESP=$(http_call POST /v1/memory/store '{"key":"integ-invalid","content":""}') || true
check_output "memory.store empty content stays typed" '"kind":"invalid_argument"' echo "$RESP"
if echo "$RESP" | grep -q '"http_status":400'; then
    check_output "memory.store empty content returns HTTP 400" "400 " echo "$RESP"
else
    echo "SKIP: memory.store empty-content HTTP mapping (runtime-web status provider is not attached)"
    SKIP=$((SKIP + 1))
fi

RESP=$(http_call POST /v1/memory/search '{}') || true
check_output "memory.search missing keywords stays typed" '"kind":"invalid_argument"' echo "$RESP"
if echo "$RESP" | grep -q '"http_status":400'; then
    check_output "memory.search missing keywords returns HTTP 400" "400 " echo "$RESP"
else
    echo "SKIP: memory.search missing-keywords HTTP mapping (runtime-web status provider is not attached)"
    SKIP=$((SKIP + 1))
fi

RESP=$(http_call POST /v1/memory/get '{}') || true
check_output "memory.get missing id stays typed" '"kind":"invalid_argument"' echo "$RESP"
if echo "$RESP" | grep -q '"http_status":400'; then
    check_output "memory.get missing id returns HTTP 400" "400 " echo "$RESP"
else
    echo "SKIP: memory.get missing-id HTTP mapping (runtime-web status provider is not attached)"
    SKIP=$((SKIP + 1))
fi

RESP=$(http_call POST /v1/memory/user_capture '{}') || true
check_output "memory.user_capture missing fields stays typed" '"kind":"invalid_argument"' echo "$RESP"
if echo "$RESP" | grep -q '"http_status":400'; then
    check_output "memory.user_capture missing fields returns HTTP 400" "400 " echo "$RESP"
else
    echo "SKIP: memory.user_capture missing-fields HTTP mapping (runtime-web status provider is not attached)"
    SKIP=$((SKIP + 1))
fi

if [ "$KB_AVAILABLE" -eq 1 ]; then
    RESP=$(srv_auth_req '{"method":"memory.store","key":"integ-test","content":"integration test value","tier":"L0","kind":"fact"}') || true
    check_output "memory.store" '"status":"ok"' echo "$RESP"
    MEM_ID=$(echo "$RESP" | python3 -c "import sys,json; print(int(json.load(sys.stdin)['id']))" 2>/dev/null) || true

    RESP=$(srv_auth_req '{"method":"memory.list","tier":"L0","limit":10}') || true
    check_output "memory.list has stored entry" "integ-test" echo "$RESP"

    RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$MEM_ID}") || true
    check_output "memory.get by ID" "integration test value" echo "$RESP"

    # `memory get --as-of` crosses client -> aimee-server -> aimee-kb, and it was
    # once broken in the middle: the server read only the id, so the flag was
    # marshalled, sent, and dropped, and the client printed the row with no
    # verdict -- which reads exactly like "not in force". Every unit test around
    # it passed, because each end was checked against a hand-written payload that
    # already contained the field. Only the real wire shows the gap, so assert it
    # here: the verdict must come back, and must NOT appear when nobody asked.
    RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$MEM_ID,\"as_of\":\"2020-01-01 00:00:00\"}") || true
    check_output "memory.get --as-of echoes the timestamp" '"as_of"' echo "$RESP"
    check_output "memory.get --as-of returns an event-time verdict" '"valid_at"' echo "$RESP"

    RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$MEM_ID}") || true
    if echo "$RESP" | grep -q '"valid_at"'; then
        check_output "memory.get without --as-of emits no verdict" "no valid_at" echo "found valid_at"
    else
        check_output "memory.get without --as-of emits no verdict" "ok" echo "ok"
    fi

    # ------------------------------------------------------------------
    # The MCP mutate verbs must not destroy a stored memory.
    #
    # tool_memory_mutate's `forget` reached a hard DELETE (row AND provenance,
    # with the audit event carrying only the id, so the content was gone for
    # good) and `update` overwrote content with no prior value kept -- neither
    # behind any capability check. Both verbs now carry MODEL authority: forget
    # retires, update supersedes.
    #
    # The unit tests cover the routing and the capability grading. Only a live
    # server shows the thing that actually matters: after the model forgets it,
    # the memory is still there. Assert it on the real wire.
    # ------------------------------------------------------------------
    RESP=$(srv_auth_req '{"method":"memory.store","key":"integ-forget","content":"value that must survive forget","tier":"L2","kind":"fact"}') || true
    check_output "memory.store (mcp forget subject)" '"status":"ok"' echo "$RESP"
    FORGET_ID=$(echo "$RESP" | python3 -c "import sys,json; print(int(json.load(sys.stdin)['id']))" 2>/dev/null) || true

    if [ -n "${FORGET_ID:-}" ]; then
        RESP=$(mcp_initialized_req "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\",\"params\":{\"name\":\"mutate\",\"arguments\":{\"verb\":\"forget\",\"id\":$FORGET_ID}}}") || true
        check_output "mcp mutate forget is allowed for an authorized caller" '"content"' echo "$RESP"
        check_output "mcp mutate forget retires rather than destroys" 'retired, not destroyed' echo "$RESP"

        # The row survives with its content intact -- a mistaken forget is
        # recoverable. This is the assertion the whole change exists for.
        RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$FORGET_ID}") || true
        check_output "a forgotten memory still exists" '"status":"ok"' echo "$RESP"
        check_output "a forgotten memory kept its content" "value that must survive forget" echo "$RESP"
    else
        echo "SKIP: mcp forget round-trip (no id from memory.store)"
        SKIP=$((SKIP + 4))
    fi

    RESP=$(srv_auth_req '{"method":"memory.store","key":"integ-update","content":"the original value","tier":"L2","kind":"fact"}') || true
    UPDATE_ID=$(echo "$RESP" | python3 -c "import sys,json; print(int(json.load(sys.stdin)['id']))" 2>/dev/null) || true

    if [ -n "${UPDATE_ID:-}" ]; then
        RESP=$(mcp_initialized_req "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\",\"params\":{\"name\":\"mutate\",\"arguments\":{\"verb\":\"update\",\"id\":$UPDATE_ID,\"content\":\"the corrected value\"}}}") || true
        check_output "mcp mutate update versions the previous value" 'previous content kept as a version' echo "$RESP"

        # The row the model edited still holds the OLD content; the new value
        # lives on a new row. An overwrite would have lost the original.
        RESP=$(srv_auth_req "{\"method\":\"memory.get\",\"id\":$UPDATE_ID}") || true
        check_output "the superseded row kept the original content" "the original value" echo "$RESP"
    else
        echo "SKIP: mcp update round-trip (no id from memory.store)"
        SKIP=$((SKIP + 2))
    fi

    # memory_maintain's prune mode bulk-deletes (every L0 row and its provenance,
    # stale L1 rows, retention-expired restricted/sensitive memories) and had no
    # gate at all. It is now graded, so confirm the gate admits an authorized
    # caller rather than bricking ordinary upkeep -- a gate that refuses everyone
    # would pass every unit test and break the running system.
    RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":32,"method":"tools/call","params":{"name":"memory_maintain","arguments":{"modes":"replay","dry_run":true}}}') || true
    check_output "mcp memory_maintain still runs for an authorized caller" '"content"' echo "$RESP"
    if echo "$RESP" | grep -qF 'insufficient capabilities'; then
        check_output "mcp memory_maintain was not refused" "ok" echo "REFUSED an authorized caller"
    else
        check_output "mcp memory_maintain was not refused" "ok" echo "ok"
    fi

    # ...but prune -- which hard-deletes in bulk -- must not run from the model's
    # door at all. Grading it was not enough: reaching any MCP tool requires
    # CAP_TOOL_EXECUTE, which only CAPS_AUTHENTICATED and CAPS_ALL carry, and
    # both of those also carry CAP_MEMORY_ADMIN. So every caller that can reach
    # this tool already clears an admin gate, and only removing prune actually
    # stops it. A bare `{}` call is the dangerous one: modes 0 means
    # MODES_DEFAULT, which includes prune.
    RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":33,"method":"tools/call","params":{"name":"memory_maintain","arguments":{"modes":"prune"}}}') || true
    check_output "mcp memory_maintain refuses an explicit prune" 'not available through this tool' echo "$RESP"

    RESP=$(mcp_initialized_req '{"jsonrpc":"2.0","id":34,"method":"tools/call","params":{"name":"memory_maintain","arguments":{"dry_run":true}}}') || true
    check_output "mcp memory_maintain drops prune from a bare call and says so" 'prune was NOT run' echo "$RESP"
else
    echo "SKIP: memory write/read round-trip (aimee-kb is not configured)"
    SKIP=$((SKIP + 6))
    echo "SKIP: mcp mutate forget/update non-destruction (aimee-kb is not configured)"
    SKIP=$((SKIP + 12))
fi

# ============================================================
# 6. Hooks through server
# ============================================================

HOOK_PAYLOAD='{"tool_name":"Read","tool_input":"{\"file_path\":\"main.c\"}"}'
set +e
RESP=$(echo "$HOOK_PAYLOAD" | CLAUDE_SESSION_ID=integ-test $AIMEE hooks pre 2>&1) || true
HOOK_RC=$?
set -e
if [ "$HOOK_RC" -ne 0 ] && echo "$RESP" | grep -q "server build mismatch"; then
    pkill -f "aimee-server.*$SOCKET" 2>/dev/null || true
    (
        cd "$REPO_ROOT/src"
        make ../aimee-server >/dev/null 2>&1
    )
    start_server || true
    set +e
    RESP=$(echo "$HOOK_PAYLOAD" | CLAUDE_SESSION_ID=integ-test $AIMEE hooks pre 2>&1) || true
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
RESP=$(http_rpc '{"method":"tool.execute","tool":"bash","arguments":"{\"command\":\"echo integ-ok\"}","session_id":"t","cwd":"/tmp","timeout_ms":5000}') || true
check_output "tool.execute bash" "integ-ok" echo "$RESP"

RESP=$(http_rpc '{"method":"tool.execute","tool":"read_file","arguments":"{\"path\":\"/etc/hostname\",\"limit\":1}","session_id":"t","cwd":"/tmp"}') || true
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
# This whole section deliberately drives SHA-256 object ids through the wire,
# the snapshot, the reconstruction and the MCP Git path (SHA-1 is covered by
# unit tests). It therefore needs a git that honours --object-format=sha256.
#
# This probe is a PRECONDITION guard, not the fix for anything observed. I added
# it believing CI's git had produced SHA-1: the snapshot there carried a 40-hex
# head with no branch or upstream, and five assertions failed about it. That was
# wrong. The probe has never fired in CI, and the section passes there -- the
# 40-hex head came from reading a DIFFERENT workspace's snapshot, which the
# selection below used to make arbitrarily. That was diagnosed independently on
# `testing` (89aa491, "address the mirror workspace's own snapshot, not the
# first one found"), which is the addressing this now uses.
#
# It stays because the precondition is real: this section drives SHA-256 ids
# deliberately, and a runner that cannot provide one should say so once rather
# than fail five assertions about a mirror that was never the problem.
MIRROR_OBJFMT=$(git -C "$MIRROR_CLIENT" rev-parse --show-object-format 2>/dev/null) || true
if [ "$MIRROR_OBJFMT" = "sha256" ]; then
    MIRROR_SHA256=1
else
    MIRROR_SHA256=0
    echo "SKIP: mirror-sync end to end (git created a '$MIRROR_OBJFMT' repository despite --object-format=sha256)"
    SKIP=$((SKIP + 22))
fi

if [ "$MIRROR_SHA256" -eq 1 ]; then
git -C "$MIRROR_CLIENT" config user.name "Aimee Integration"
git -C "$MIRROR_CLIENT" config user.email "aimee-integration@example.invalid"
printf 'base\n' >"$MIRROR_CLIENT/file.txt"
git -C "$MIRROR_CLIENT" add file.txt
git -C "$MIRROR_CLIENT" commit -m base >/dev/null
git -C "$MIRROR_CLIENT" remote add origin "$MIRROR_REMOTE"
git -C "$MIRROR_CLIENT" push -u origin feature.locked >/dev/null
MIRROR_HEAD=$(git -C "$MIRROR_CLIENT" rev-parse HEAD) || true
printf 'client edit\n' >>"$MIRROR_CLIENT/file.txt"
git -C "$MIRROR_CLIENT" diff --binary HEAD >"$MIRROR_CASE/client.diff"

ADD_REQ=$(python3 - "$MIRROR_CLIENT" "$MIRROR_REMOTE" "$MIRROR_HEAD" <<'PY'
import json, sys
print(json.dumps({"method": "workspace.add", "root": sys.argv[1], "provider": "mirror",
                  "remote": sys.argv[2], "head": sys.argv[3], "scan": False}))
PY
)
RESP=$(http_rpc "$ADD_REQ") || true
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
RESP=$(http_rpc "${MIRROR_REQS[0]}") || true
check_output "mirror-sync begin persisted" '"order":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[1]}") || true
check_output "mirror-sync continuation persisted" '"seq":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[2]}") || true
check_output "mirror-sync final published" '"generation":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[3]}") || true
check_output "identical mirror-sync begin advances order" '"order":2' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[4]}") || true
check_output "identical mirror-sync continuation persisted" '"seq":1' echo "$RESP"
RESP=$(http_rpc "${MIRROR_REQS[5]}") || true
check_output "identical mirror-sync reuses generation" '"generation":1' echo "$RESP"

# Address the mirror workspace by its own snapshot, not by whichever one find
# happens to walk into first. Every check below derives from $SNAPSHOT -- its
# directory, its generation, its work-N-* checkouts -- so picking a different
# workspace's file does not fail here, it fails five checks later with content
# that looks like a mirror bug. The server keys the directory on
# fnv1a_hex8(root) (workspace_mirror.c), so the harness can name it exactly.
MIRROR_HASH=$(python3 - "$MIRROR_CLIENT" <<'HASH'
import sys
h = 2166136261
for byte in sys.argv[1].encode():
    h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
print("%08x" % h)
HASH
)
SNAPSHOT="$AIMEE_HOME/workspaces/$MIRROR_HASH/client.snapshot"
if [ ! -s "$SNAPSHOT" ]; then
    # Show what WAS published. Without this the five checks below explain the
    # absence one confusing assertion at a time -- which is how this bug
    # presented in the first place, as a mirror that looked broken.
    echo "  no snapshot at $SNAPSHOT; published snapshots were:"
    find "$AIMEE_HOME/workspaces" -name client.snapshot -type f -exec sh -c \
        'echo "    $1: $(cat "$1")"' _ {} \; 2>/dev/null
fi
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
RESP=$(mcp_initialized_req "$GIT_REQ") || true
check_output "MCP Git resolves reconstructed .locked branch" 'feature.locked' echo "$RESP"
check_output "MCP Git sees synchronized client edit" 'file.txt' echo "$RESP"

MIRROR_WORK=$(find "$(dirname "$SNAPSHOT")" -maxdepth 1 -type d -name 'work-1-*' -print -quit) || true
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
RESP=$(http_rpc "$EMPTY_REQ") || true
check_output "clean mirror-sync publishes a new generation" '"generation":2' echo "$RESP"

RESP=$(mcp_initialized_req "$GIT_REQ") || true
check_output "first MCP Git call accepts clean snapshot" 'feature.locked' echo "$RESP"
MIRROR_CLEAN_WORK=$(find "$(dirname "$SNAPSHOT")" -maxdepth 1 -type d -name 'work-2-*' \
    -print -quit)
check "clean snapshot worktree materialized on first call" test -n "$MIRROR_CLEAN_WORK"
check "clean snapshot worktree remains clean" test -z \
    "$(git -C "$MIRROR_CLEAN_WORK" status --porcelain)"

fi  # MIRROR_SHA256

# ============================================================
# 10. Workflow control plane
# ============================================================
# The daemon no longer owns lifecycle state -- every read and write below
# crosses the module bus and is answered by aimee-module-db. That makes these
# checks the end-to-end proof of the migration: not "the store works" (the unit
# suite covers that in isolation) but "the routes a client actually calls still
# behave when their state lives in another process".

WF_STEP=""
if [ "$WORKFLOW_MODULE_INSTALLED" -ne 1 ]; then
    WF_STEP="install (go toolchain, build, or generated grants)"
elif ! start_workflow_module; then
    WF_STEP="attach (the module did not answer /v1/workflow/defs with 200; last: $(http_call GET /v1/workflow/defs))"
else
    WORKFLOW_MODULE_READY=1
fi

if [ "$WORKFLOW_MODULE_READY" -ne 1 ]; then
    echo "SKIP: the workflow control module could not be started here: $WF_STEP"
    echo "      (needs a Go toolchain and the generated wfe/workflows grants);"
    echo "      the workflow surface is covered by the full-stack E2E instead."
    # Only ask what the module SAID when it actually ran. On the install path it
    # never started, and "the module wrote nothing" would report silence from a
    # process that was never there -- which is the reader's next hour wasted.
    if [ -s "$HOME/aimee-wfe.log" ]; then
        echo "      what the module said:"
        tail -15 "$HOME/aimee-wfe.log" | sed 's/^/      /'
    elif [ -f "$HOME/aimee-wfe.log" ]; then
        echo "      the module started and wrote nothing to $HOME/aimee-wfe.log"
    fi
    SKIP=$((SKIP + 32))
else

check_output "workflow defs list" '"defs"' echo "$(http_call GET /v1/workflow/defs)"
check_output "the shipped definitions are served" 'build' echo "$(http_call GET /v1/workflow/defs)"
check_output "workflow items list answers" '200' echo "$(http_status GET /v1/workflow/items)"
check_output "workflow triggers list answers" '200' echo "$(http_status GET /v1/workflow/triggers)"
check_output "workflow blocks list answers" '200' echo "$(http_status GET /v1/workflow/blocks)"

# Intake refuses before it records. Both halves are required: a run with nothing
# to work on, and a run with nowhere to do it, are equally unstartable.
check_output "submit without a proposal is refused" '400' \
    echo "$(http_status POST /v1/dev/submit '{"repo":"integ/repo"}')"
check_output "submit without a repo is refused too" '400' \
    echo "$(http_status POST /v1/dev/submit '{"proposal_md":"# no repo"}')"

WF_SUB1=$(http_call POST /v1/dev/submit '{"proposal_md":"# integ one\n\ndo the first thing","workflow":"build","repo":"integ/repo"}')
check_output "a submit is admitted" '200' echo "${WF_SUB1%% *}"
check_output "and it names the run it started" '"work_item_id"' echo "$WF_SUB1"
WI1=$(echo "$WF_SUB1" | sed -n 's/.*"work_item_id"[^"]*"\([^"]*\)".*/\1/p')
check "the submit returned an id" test -n "$WI1"

WF_SUB2=$(http_call POST /v1/dev/submit '{"proposal_md":"# integ two\n\ndo the second thing","workflow":"build","repo":"integ/repo"}')
check_output "a second submit is admitted" '200' echo "${WF_SUB2%% *}"
WI2=$(echo "$WF_SUB2" | sed -n 's/.*"work_item_id"[^"]*"\([^"]*\)".*/\1/p')
check "the two runs are distinct" test -n "$WI2" -a "$WI1" != "$WI2"

# Each run owns its proposal. Two runs sharing one artifact would mean the later
# submit silently replaced the earlier one's instructions, and the first run
# would then execute work nobody asked for -- a wrong answer, not an error.
check "the first run's proposal was stored" test -s "$AIMEE_HOME/wfe-artifacts/$WI1/proposal.md"
check "the second run's proposal was stored separately" \
    test -s "$AIMEE_HOME/wfe-artifacts/$WI2/proposal.md"
check_output "and the first still says what it said" 'do the first thing' \
    cat "$AIMEE_HOME/wfe-artifacts/$WI1/proposal.md"
check_output "while the second says its own thing" 'do the second thing' \
    cat "$AIMEE_HOME/wfe-artifacts/$WI2/proposal.md"

# Admission is capped. Submit until something refuses rather than assuming which
# attempt crosses the line -- the cap is a policy value, and a test that hard-codes
# "the third one" fails for the wrong reason the day the default moves.
WF_CAP_CODE="" ; WF_CAP_BODY=""
for i in 1 2 3 4 5 6 7 8; do
    WF_C=$(http_call POST /v1/dev/submit "{\"proposal_md\":\"# cap probe $i\",\"workflow\":\"build\",\"repo\":\"integ/repo\"}")
    WF_CAP_CODE="${WF_C%% *}"
    WF_CAP_BODY="$WF_C"
    [ "$WF_CAP_CODE" != "200" ] && break
done
check_output "admission is capped, and refuses rather than admitting forever" '409' echo "$WF_CAP_CODE"
check_output "and the refusal says the cap refused it" 'admission full' echo "$WF_CAP_BODY"

# The read side. Written through the bus, read back through the bus.
check_output "the run is listed" "$WI1" echo "$(http_call GET /v1/workflow/items)"
check_output "the run can be fetched by id" "$WI1" echo "$(http_call GET /v1/workflow/items/$WI1)"
check_output "the run reports a stage" '"stage"' echo "$(http_call GET /v1/workflow/items/$WI1)"
check_output "the run reports its submitter" '"submitter"' echo "$(http_call GET /v1/workflow/items/$WI1)"
check_output "the run's events are served" '200' echo "$(http_status GET /v1/workflow/items/$WI1/events)"
check_output "the run's proposal is served" '200' echo "$(http_status GET /v1/workflow/items/$WI1/proposal)"
check_output "an unknown run is not found" '404' echo "$(http_status GET /v1/workflow/items/wi_no_such_run)"

# How far a run gets with no runner configured, which is the state of this
# harness: it is admitted, it is driven, and it PARKS naming what it lacked. That
# is the behaviour worth pinning -- an intake that admitted a run and then left it
# silently "active" forever would look identical from the outside on the day the
# runner really was broken.
WF_PARKED=0
for i in $(seq 1 50); do
    case "$(http_call GET /v1/workflow/items/$WI1)" in
        *runner_unavailable*) WF_PARKED=1; break ;;
    esac
    sleep 0.2
done
check "with no runner configured the run parks instead of stalling silently" \
    test "$WF_PARKED" -eq 1
check_output "and it parks naming what it was waiting for" 'runner_unavailable' \
    echo "$(http_call GET /v1/workflow/items/$WI1)"

# Resume is not a blanket override. A park the operator did not cause, and cannot
# clear by deciding, is refused rather than quietly re-arming a run whose blocker
# is still there.
check_output "resuming a park the operator cannot clear is refused" '409' \
    echo "$(http_status POST /v1/workflow/items/$WI1/resume '{}')"

# Stopping ends the run and frees the admission slot it held -- the observable
# consequence that matters, since a cap that never released would wedge intake.
check_output "the run can be stopped" '200' \
    echo "$(http_status POST /v1/workflow/items/$WI1/stop '{}')"
WF_SUB4=$(http_call POST /v1/dev/submit '{"proposal_md":"# integ four\n\nafter a slot freed","workflow":"build","repo":"integ/repo"}')
check_output "stopping a run frees the admission slot it held" '200' echo "${WF_SUB4%% *}"

check_output "a run can be deleted" '200' echo "$(http_status DELETE /v1/workflow/items/$WI2)"
check_output "and it is gone from the read side" '404' \
    echo "$(http_status GET /v1/workflow/items/$WI2)"

stop_workflow_module

fi  # WORKFLOW_MODULE_READY

# ============================================================
# 11. Server shutdown
# ============================================================

kill "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

check "socket removed after shutdown" test ! -S "$HTTP_SOCK"

# ============================================================
# Results
# ============================================================

REACHED_SUMMARY=1
TOTAL=$((PASS + FAIL)) || true
echo ""
echo "integration: $PASS/$TOTAL passed"
if [ "$SKIP" -gt 0 ]; then
    echo "SKIPPED ($SKIP checks whose service is not configured here; covered by full-stack E2E)"
fi
if [ "$FAIL" -gt 0 ]; then
    echo "FAILED ($FAIL failures)"
    exit 1
fi
echo "All integration tests passed."
