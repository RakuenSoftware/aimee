#!/bin/bash
# test_index_scan_e2e.sh: end-to-end pipeline test for `aimee index scan`.
#
# Pins the no-silent-swallow contract through the full
# aimee-client -> aimee-server -> aimee-kb path. A fake aimee-kb returns
# canned responses on a per-test-isolated socket; the real aimee-client
# and aimee-server binaries handle dispatch and rendering.
#
# Cases:
#   1. kb returns {status:error,message:"..."}     -> rc!=0, stderr carries
#                                                     the kb message, no
#                                                     "Scan complete" line.
#   2. kb returns {status:ok,projects:N,files:M}   -> rc=0, "Scan complete:
#                                                     N project(s), M file(s)".
#   3. kb returns {status:ok,skipped:true,
#                  reason:cooldown,retry_after:42} -> rc=0, "cooldown active
#                                                     (42s remaining)".
#
# Isolation rules — this script must NOT touch any aimee process or file
# outside its own per-case sandbox:
#   - HOME is overridden per-invocation via `env HOME=...` (no `export`).
#     The surrounding shell's HOME, AIMEE_SOCK, AIMEE_*, etc. are stripped
#     by `env -i` from every aimee subprocess we launch.
#   - aimee-server is started EXPLICITLY with a known --socket so we own
#     its PID and never have to grep for processes by name.
#   - The client targets that exact socket via AIMEE_SOCK in its scoped
#     env, so auto-spawn is bypassed.
#   - The fake-kb python script lives in the per-case TMPDIR; nothing
#     leaks to /tmp on success or failure.
#   - All process kills go through captured PIDs. No pkill, no pgrep.
#   - A trap fires on interrupt or unexpected exit to guarantee cleanup.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
AIMEE_CLIENT="$REPO_ROOT/aimee"
AIMEE_SERVER_BIN="$REPO_ROOT/aimee-server"

if [ ! -x "$AIMEE_CLIENT" ] || [ ! -x "$AIMEE_SERVER_BIN" ]; then
    echo "test_index_scan_e2e: missing binaries (run \`make\` first)"
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "test_index_scan_e2e: SKIP (python3 not available)"
    exit 0
fi

PASS=0
FAIL=0
ACTIVE_TMPHOME=""
ACTIVE_SERVER_PID=""
ACTIVE_KB_PID=""

cleanup_active() {
    # Reap both children before removing the home they write into. kill only
    # requests exit, so without the waits a child can still be writing under
    # $ACTIVE_TMPHOME while rm -rf walks it, and rm fails with ENOTEMPTY when a
    # directory gains entries between unlinking its children and removing it.
    if [ -n "$ACTIVE_SERVER_PID" ]; then
        kill "$ACTIVE_SERVER_PID" 2>/dev/null || true
        wait "$ACTIVE_SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$ACTIVE_KB_PID" ]; then
        kill "$ACTIVE_KB_PID" 2>/dev/null || true
        wait "$ACTIVE_KB_PID" 2>/dev/null || true
    fi
    if [ -n "$ACTIVE_TMPHOME" ] && [ -d "$ACTIVE_TMPHOME" ]; then
        # `|| true` regardless: teardown of a temporary directory must never
        # decide the exit status. Even with the waits, anything else holding a
        # file open here would taint a run that has nothing left to do.
        rm -rf "$ACTIVE_TMPHOME" || true
    fi
    ACTIVE_TMPHOME=""
    ACTIVE_SERVER_PID=""
    ACTIVE_KB_PID=""
}
trap cleanup_active EXIT INT TERM

# Wait up to ~5s for a Unix socket to appear at $1.
wait_for_socket() {
    local path="$1"
    local i
    for i in $(seq 1 100); do
        [ -S "$path" ] && return 0
        sleep 0.05
    done
    return 1
}

run_case() {
    local desc="$1"
    local scan_resp_json="$2"
    local expected_rc="$3"
    local expected_substring="$4"
    local forbidden_substring="$5"

    local tmphome
    tmphome=$(mktemp -d /tmp/aimee-index-scan-e2e-XXXXXX)
    ACTIVE_TMPHOME="$tmphome"
    mkdir -p "$tmphome/.config/aimee"

    cat > "$tmphome/.config/aimee/aimee.yaml" <<EOF
guardrail_mode: approve
provider: claude
EOF

    local kb_sock="$tmphome/.config/aimee/aimee-kb.sock"
    # aimee-server now serves only /v1; its UDS is aimee-http.sock.
    local server_sock="$tmphome/.config/aimee/aimee-http.sock"
    local scan_resp_file="$tmphome/scan-resp.json"
    local fake_kb_py="$tmphome/fake-kb.py"
    local kb_log="$tmphome/kb.log"
    local server_log="$tmphome/server.log"

    printf '%s\n' "$scan_resp_json" > "$scan_resp_file"

    cat > "$fake_kb_py" <<'PY'
import socket, json, sys, os, threading

sock_path, scan_resp_path = sys.argv[1], sys.argv[2]
try: os.unlink(sock_path)
except FileNotFoundError: pass

with open(scan_resp_path) as f:
    SCAN_RESP = f.read().strip()

srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path)
os.chmod(sock_path, 0o600)
srv.listen(8)

def serve(conn):
    try:
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = conn.recv(65536)
            if not chunk: return
            buf += chunk
        req = json.loads(buf.decode().strip())
        method = req.get("method", "")
        if method == "server.info":
            resp = {"status":"ok","protocol_version":1,
                    "server_version":"fake","service":"aimee-kb"}
        elif method == "index.scan":
            conn.sendall((SCAN_RESP + "\n").encode())
            return
        else:
            resp = {"status":"error","message":"fake kb: unknown method " + method}
        conn.sendall((json.dumps(resp) + "\n").encode())
    finally:
        conn.close()

while True:
    try:
        c, _ = srv.accept()
    except OSError:
        break
    threading.Thread(target=serve, args=(c,), daemon=True).start()
PY

    python3 "$fake_kb_py" "$kb_sock" "$scan_resp_file" >"$kb_log" 2>&1 &
    ACTIVE_KB_PID=$!

    if ! wait_for_socket "$kb_sock"; then
        echo "FAIL: $desc (fake kb failed to start)"
        echo "  fake kb log:"
        sed 's/^/    /' "$kb_log" 2>/dev/null || true
        cleanup_active
        FAIL=$((FAIL + 1))
        return
    fi

    # `env -i` strips the surrounding shell environment so the spawned
    # server can never inherit a stray HOME / AIMEE_SOCK / AIMEE_*.
    env -i HOME="$tmphome" PATH="$PATH" \
        "$AIMEE_SERVER_BIN" --socket="$server_sock" --log-level=info \
        >"$server_log" 2>&1 &
    ACTIVE_SERVER_PID=$!

    if ! wait_for_socket "$server_sock"; then
        echo "FAIL: $desc (aimee-server failed to start)"
        echo "  server log:"
        sed 's/^/    /' "$server_log" 2>/dev/null || true
        cleanup_active
        FAIL=$((FAIL + 1))
        return
    fi

    # AIMEE_SOCK pins the client to OUR server.
    local output rc
    output=$(env -i HOME="$tmphome" PATH="$PATH" AIMEE_SOCK="$server_sock" \
        "$AIMEE_CLIENT" index scan 2>&1)
    rc=$?

    cleanup_active

    local case_failed=0
    if [ "$rc" -ne "$expected_rc" ]; then
        echo "FAIL: $desc"
        echo "  expected rc=$expected_rc, got rc=$rc"
        echo "  output: $output"
        case_failed=1
    fi
    if [ -n "$expected_substring" ] && ! echo "$output" | grep -qF "$expected_substring"; then
        echo "FAIL: $desc"
        echo "  expected substring not found: $expected_substring"
        echo "  output: $output"
        case_failed=1
    fi
    if [ -n "$forbidden_substring" ] && echo "$output" | grep -qF "$forbidden_substring"; then
        echo "FAIL: $desc"
        echo "  forbidden substring present: $forbidden_substring"
        echo "  output: $output"
        case_failed=1
    fi

    if [ $case_failed -eq 0 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
}

# --- Cases ------------------------------------------------------------

# 1. The exact regression: kb returns status:error. CLI must surface the
#    message and exit non-zero. Must NOT print "Scan complete: 0".
run_case "kb error surfaces to user" \
    '{"status":"error","message":"canonical index unavailable (DB2 not initialized)"}' \
    1 \
    "DB2 not initialized" \
    "Scan complete"

# 2. Healthy success path. CLI prints the scan summary and exits 0.
run_case "kb success prints scan summary" \
    '{"status":"ok","skipped":false,"projects":3,"files":42}' \
    0 \
    "Scan complete: 3 project(s), 42 file(s)" \
    ""

# 3. Cooldown skip. CLI prints the "skipped" line (with seconds remaining)
#    and exits 0 — this is informational, not a failure.
run_case "kb cooldown surfaces retry_after" \
    '{"status":"ok","skipped":true,"reason":"cooldown","retry_after":42}' \
    0 \
    "cooldown active (42s remaining)" \
    "Scan complete"

echo ""
echo "test_index_scan_e2e: $PASS passed, $FAIL failed."
[ "$FAIL" -eq 0 ] || exit 1
