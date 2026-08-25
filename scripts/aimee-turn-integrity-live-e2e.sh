#!/usr/bin/env bash
# Invoked by aimee-local-stack-e2e.sh while every real stack component is live.
set -euo pipefail

: "${REPO:?} ${RUN_ROOT:?} ${SCRATCH:?} ${SERVER_URL:?} ${BEARER:?}"
: "${CLIENT_CERT:?} ${CLIENT_KEY:?} ${KB_PID:?}"

pass=0
fail() { echo "turn-integrity-live-e2e: FAIL: $*" >&2; exit 1; }
ok() { echo "  PASS  $*"; pass=$((pass + 1)); }

provider_log="$RUN_ROOT/turn-integrity-provider.jsonl"
TI_FIXTURE_LOG="$provider_log" python3 "$REPO/scripts/fixtures/turn-integrity-openai.py" \
  >"$RUN_ROOT/turn-integrity-provider.log" 2>&1 &
provider_pid=$!
cleanup_provider() {
  kill "$provider_pid" 2>/dev/null || true
  wait "$provider_pid" 2>/dev/null || true
}
trap cleanup_provider EXIT

for _ in $(seq 1 50); do
  curl -fsS http://127.0.0.1:18991/v1/models >/dev/null 2>&1 && break
  kill -0 "$provider_pid" 2>/dev/null || fail "fixture provider exited"
  sleep 0.1
done
curl -fsS http://127.0.0.1:18991/v1/models >/dev/null || fail "fixture provider unavailable"
ok "deterministic provider is reachable over HTTP"

client_home="$SCRATCH/client"
AIMEE_HOME="$client_home" "$REPO/aimee" agent local turn-integrity-fixture \
  http://127.0.0.1:18991/v1 --model turn-integrity-fixture --slots 1 --ctx 32768 \
  --timeout-ms 5000 --no-probe --no-fallback >/dev/null
ok "fixture model registered through the live server"

uds="$SCRATCH/aimee-http.sock"
set_primary() {
  local sid="$1"
  curl -fsS --unix-socket "$uds" -H 'content-type: application/json' -X POST \
    -d '{"agent":"turn-integrity-fixture"}' \
    "http://localhost/v1/sessions/${sid}/primary" >/dev/null
}

run_turn() {
  local sid="$1" prompt="$2" cwd="$3" out="$4"
  set_primary "$sid"
  python3 - "$sid" "$prompt" "$cwd" <<'PY' >"$out"
import json
import os
import socket
import sys

sid, prompt, cwd = sys.argv[1:]
body = json.dumps({
    "method": "chat.send_stream",
    "message": prompt,
    "cwd": cwd,
    "aimee_session_id": sid,
    "client_type": "turn-integrity-e2e",
})
request = (
    "POST /v1/chat/stream HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    f"Content-Length: {len(body.encode())}\r\n\r\n{body}"
).encode()
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(45)
sock.connect(os.path.join(os.environ["SCRATCH"], "aimee-http.sock"))
sock.sendall(request)
chunks = []
while True:
    data = sock.recv(65536)
    if not data:
        break
    chunks.append(data)
sys.stdout.buffer.write(b"".join(chunks))
PY
  grep -q '"event":"done"' "$out" || fail "turn $sid did not finish"
}

workspace="$RUN_ROOT/turn-integrity-workspace"
mkdir -p "$workspace"
run_turn ti-live-write TI_WRITE_FILE "$workspace" "$RUN_ROOT/write-turn.out"
[[ "$(<"$workspace/turn-integrity-live.txt")" == "live-contract-ok" ]] || \
  fail "write_file did not produce the expected bytes"
ok "model-backed write_file executed with exact readback"

run_turn ti-live-edit TI_EDIT_FILE "$workspace" "$RUN_ROOT/edit-turn.out"
[[ "$(<"$workspace/turn-integrity-live.txt")" == "live-contract-edited" ]] || \
  fail "edit_file did not produce the expected bytes"
ok "model-backed edit_file executed with exact readback"

run_turn ti-live-empty TI_SEARCH_EMPTY "$workspace" "$RUN_ROOT/search-turn.out"
python3 - "$provider_log" <<'PY' || fail "typed empty retrieval contract did not cross provider seam"
import json
import sys

wire = "\n".join(open(sys.argv[1], encoding="utf-8"))
required = [
    '\\"status\\":\\"empty\\"',
    '\\"action\\":\\"web_search\\"',
    '\\"policy_recheck\\":true',
    '\\"authorized\\":false',
]
raise SystemExit(0 if all(token in wire for token in required) else 1)
PY
ok "typed empty retrieval and inert continuation crossed server→KB→agent"

# Safe external mutation: the remote is a disposable bare repository in this CT.
git_work="$RUN_ROOT/turn-integrity-git"
git_bare="$RUN_ROOT/turn-integrity-remote.git"
git init -q --bare "$git_bare"
git init -q -b main "$git_work"
git -C "$git_work" config user.name aimee-e2e
git -C "$git_work" config user.email aimee-e2e@example.invalid
printf 'live\n' >"$git_work/live.txt"
git -C "$git_work" add live.txt
git -C "$git_work" commit -qm initial
git -C "$git_work" remote add origin "$git_bare"
run_turn ti-live-push TI_GIT_PUSH "$git_work" "$RUN_ROOT/push-turn.out"
git --git-dir="$git_bare" show-ref --verify --quiet refs/heads/main || \
  fail "authorized git_push did not reach the disposable remote"
ok "authorized external git_push reached a disposable real remote"

# Observe one session, invalidate knowledge through the public API, then observe
# the same session again. The second provider request must carry the stale marker.
fresh_sid=ti-live-freshness
run_turn "$fresh_sid" TI_FRESHNESS_BEFORE "$workspace" "$RUN_ROOT/fresh-before.out"
curl -fksS --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
  -H "Authorization: Bearer $BEARER" -H 'content-type: application/json' -X POST \
  -d '{"source_kind":"repository","source_id":"turn-integrity-e2e"}' \
  "$SERVER_URL/v1/curator/invalidated" >/dev/null
run_turn "$fresh_sid" TI_FRESHNESS_AFTER "$workspace" "$RUN_ROOT/fresh-after.out"
grep -q 'aimee-freshness' "$provider_log" || \
  fail "knowledge invalidation did not add the freshness marker"
ok "live invalidation produced a stale-knowledge instruction on the next turn"

# Run and store a real evaluation against the registered fixture model.
AIMEE_HOME="$client_home" "$REPO/aimee" eval run \
  "$REPO/scripts/fixtures/turn-integrity-eval" --seed 4242 >/dev/null
eval_json="$(AIMEE_HOME="$client_home" "$REPO/aimee" --json eval results \
  "$REPO/scripts/fixtures/turn-integrity-eval")"
[[ "$eval_json" == *'turn integrity live fixture'* ]] || fail "stored eval row missing"
ok "live benchmark execution stored its result"

python3 - "$SCRATCH/audit/worm-live.db" <<'PY' || fail "turn/effect audit evidence missing"
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
rows = db.execute("select action, detail from audit_event order by seq").fetchall()
actions = {row[0] for row in rows}
details = "\n".join(row[1] or "" for row in rows)
required = {
    "turn.created",
    "turn.state",
    "effect.proposed",
    "effect.validated",
    "effect.executing",
    "effect.postcondition",
    "effect.completed",
    "knowledge.invalidated",
}
if not required.issubset(actions):
    raise SystemExit(1)
if "live-contract-ok" in details or "turn-integrity-live.txt" in details:
    raise SystemExit(1)
PY
ok "WORM contains bounded turn/effect/freshness lifecycles without raw arguments"

# Freeze the real KB process, bound the failed turn, then resume and prove health.
kill -STOP "$KB_PID"
run_turn ti-live-kb-down TI_SEARCH_EMPTY "$workspace" "$RUN_ROOT/kb-down-turn.out" || true
kill -CONT "$KB_PID"
for _ in $(seq 1 50); do
  curl -fsS http://127.0.0.1:8741/v1/health >/dev/null 2>&1 && break
  sleep 0.1
done
curl -fsS http://127.0.0.1:8741/v1/health >/dev/null || fail "KB did not recover after resume"
ok "KB transport failure was bounded and the live KB recovered"

echo "turn-integrity-live-e2e: $pass checks passed"
