#!/bin/bash
# test_cli.sh: smoke tests for the aimee CLI
# Run from the repo root: ./src/tests/test_cli.sh
# Commands must route through typed server/kb RPCs.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

AIMEE="$REPO_ROOT/aimee"
AIMEE_SERVER="$REPO_ROOT/aimee-server"
TEST_HOME=$(mktemp -d /tmp/aimee-test-home-XXXXXX)
export HOME="$TEST_HOME"
export AIMEE_HOME="$HOME/.config/aimee"
export AIMEE_SOCK="$AIMEE_HOME/aimee.sock"
unset AIMEE_PROFILE
mkdir -p "$AIMEE_HOME"

SERVER_PID=""
SERVER_LOG="$HOME/server.log"

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$HOME"
}
trap cleanup EXIT

wait_for_socket() {
    local sock="$1"
    local i
    for i in $(seq 1 100); do
        [ -S "$sock" ] && return 0
        sleep 0.05
    done
    return 1
}

wait_for_server() {
    local i
    for i in $(seq 1 100); do
        "$AIMEE" status >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}

PASS=0
FAIL=0

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

check_output_not_contains() {
    local desc="$1"
    local unexpected="$2"
    shift 2
    local output
    output=$("$@" 2>&1) || true
    if echo "$output" | grep -qF "$unexpected"; then
        echo "FAIL: $desc (unexpected '$unexpected', got '$(echo "$output" | head -1)')"
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi
}

require_binary "$AIMEE"
require_binary "$AIMEE_SERVER"

"$AIMEE_SERVER" --foreground >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
if ! wait_for_socket "$AIMEE_HOME/aimee-http.sock"; then
    echo "FAIL: server start"
    sed 's/^/  /' "$SERVER_LOG" 2>/dev/null || true
    exit 1
fi
if ! wait_for_server; then
    echo "FAIL: server ready"
    sed 's/^/  /' "$SERVER_LOG" 2>/dev/null || true
    exit 1
fi

# --- Basic commands ---
check "version" $AIMEE version
check "version flag" $AIMEE --version
check_output "version --json" "\"version\"" $AIMEE --json version

# --- Memory (read-only baseline; write/KB routing lives in service tests) ---
check_output "memory list json" "[" $AIMEE --json memory list
check_output "memory read json" "\"context\"" $AIMEE --json memory read

# --- Index ---
check_output "index overview json" "[" $AIMEE --json index overview
check_output "index overview trailing json" "[" $AIMEE index overview --json

# --- Trigger ---
check_output "trigger fire json" "\"trigger_id\"" \
    $AIMEE --json trigger fire --source cli-test --task "smoke trigger dispatch"
check_output "trigger list json" "\"source\":\"cli-test\"" $AIMEE --json trigger list

# --- Agent ---
check_output "agent list (empty)" "[]" $AIMEE --json agent list
check "agent local no-probe" $AIMEE agent local gemma-local http://127.0.0.1:8080 \
    --model gemma-test --slots 4 --ctx 131072 --no-probe
check_output "agent local max_parallel" "\"max_parallel\":4" $AIMEE --json agent list
check_output "delegate list roles routes" "gemma-local" $AIMEE delegate --list-roles
check_output "agent local context" "\"context_window\":131072" $AIMEE --json agent list
check_output "agent probe configured no-run" "\"detected_slots\"" \
    $AIMEE --json agent probe gemma-local --no-run
check "agent add hosted" $AIMEE agent add hosted http://127.0.0.1:9/v1 hosted-model \
    --provider openrouter --key dummy --max-parallel 7
check_output "hosted agent probe uses configured slots" "\"slots_source\":\"config\"" \
    $AIMEE --json agent probe hosted --no-run
check_output "hosted agent probe configured parallel" "\"detected_slots\":7" \
    $AIMEE --json agent probe hosted --no-run
check "agent remove hosted" $AIMEE agent remove hosted
check "agent disable" $AIMEE agent disable gemma-local
check_output "agent disable reflected" "\"enabled\":false" $AIMEE --json agent list
check "agent enable" $AIMEE agent enable gemma-local
check_output "agent enable reflected" "\"enabled\":true" $AIMEE --json agent list
check "agent remove" $AIMEE agent remove gemma-local
check_output "agent list after remove" "[]" $AIMEE --json agent list

# --- Jobs ---
check_output "jobs list" "No jobs." $AIMEE jobs list
check_output "coord job list" "No coordinated jobs found." $AIMEE job list

# --- Help ---
check_output "help (no args)" "Usage:" $AIMEE help
check_output "help memory" "Subcommands:" $AIMEE help memory
check_output "help agent" "Subcommands:" $AIMEE help agent
check_output "help jobs" "Durable delegate job inspection" $AIMEE help jobs
check_output "help job" "Coordinated parallel job management" $AIMEE help job
check_output "help index" "Subcommands:" $AIMEE help index
check_output "delegate role help" "Usage: aimee delegate <role>" $AIMEE delegate review --help
check_output "delegate status help" "Usage: aimee delegate status <job_id> [job_id...]" $AIMEE delegate status --help
check_output "delegate status missing id" "Usage: aimee delegate status <job_id>" $AIMEE delegate status
check_output "delegate status multiple ids" "job_id: 2" $AIMEE delegate status 1 2
check_output "help version" "Print version" $AIMEE help version
check_output "--help" "Commands:" $AIMEE --help
check_output "no subcommand prints usage" "Commands:" $AIMEE
check_output "memory (no subcommand)" "Subcommands:" $AIMEE memory
check_output "index (no subcommand)" "Subcommands:" $AIMEE index

echo ""
echo "$((PASS + FAIL)) tests: $PASS passed, $FAIL failed."
[ "$FAIL" -eq 0 ] || exit 1
