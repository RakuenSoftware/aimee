#!/usr/bin/env bash
# Invoked by aimee-local-stack-e2e.sh while every real stack component is live.
set -euo pipefail

: "${REPO:?} ${RUN_ROOT:?} ${SCRATCH:?} ${SERVER_URL:?} ${BEARER:?}"
: "${CLIENT_CERT:?} ${CLIENT_KEY:?} ${KB_PID:?}"

pass=0
fail() { echo "turn-integrity-live-e2e: FAIL: $*" >&2; exit 1; }
ok() { echo "  PASS  $*"; pass=$((pass + 1)); }

provider_log="$RUN_ROOT/turn-integrity-provider.jsonl"
provider_port=$((19000 + RANDOM % 1000))
provider_url="http://127.0.0.1:${provider_port}/v1"
TI_FIXTURE_LOG="$provider_log" TI_FIXTURE_PORT="$provider_port" \
  python3 "$REPO/scripts/fixtures/turn-integrity-openai.py" \
  >"$RUN_ROOT/turn-integrity-provider.log" 2>&1 &
provider_pid=$!
kb_stopped=0
cleanup_provider() {
  if (( kb_stopped )); then
    kill -CONT "$KB_PID" 2>/dev/null || true
  fi
  kill "$provider_pid" 2>/dev/null || true
  wait "$provider_pid" 2>/dev/null || true
}
trap cleanup_provider EXIT

for _ in $(seq 1 50); do
  curl -fsS "$provider_url/models" >/dev/null 2>&1 && break
  kill -0 "$provider_pid" 2>/dev/null || fail "fixture provider exited"
  sleep 0.1
done
curl -fsS "$provider_url/models" >/dev/null || fail "fixture provider unavailable"
ok "deterministic provider is reachable over HTTP"

client_home="$SCRATCH/client"
AIMEE_HOME="$client_home" "$REPO/aimee" agent local turn-integrity-fixture \
  "$provider_url" --model turn-integrity-fixture --slots 1 --ctx 32768 \
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
  # A real CPU embedder also persists post-turn feedback before the stream closes.
  # On the acceptance CT that takes tens of seconds in addition to model/tool I/O;
  # keep the client bound above the product's dependency timeout without making
  # it unbounded. Callers may still supply a tighter, purpose-specific budget.
  local sid="$1" prompt="$2" cwd="$3" out="$4" socket_timeout="${5:-150}"
  set_primary "$sid"
  python3 - "$sid" "$prompt" "$cwd" "$socket_timeout" <<'PY' >"$out"
import json
import os
import socket
import sys

sid, prompt, cwd, socket_timeout = sys.argv[1:]
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
sock.settimeout(float(socket_timeout))
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
  grep -q '"event":"done"' "$out" || {
    sed -n '1,120p' "$out" >&2
    fail "turn $sid did not finish"
  }
}

# Dense retrieval always has a nearest neighbor once any feedback has been
# stored. Prove the typed empty-corpus contract before the first model turn can
# write feedback memories, rather than manufacturing an impossible query later.
empty_workspace="$RUN_ROOT/turn-integrity-empty-workspace"
mkdir -p "$empty_workspace"
run_turn ti-live-empty TI_SEARCH_EMPTY "$empty_workspace" "$RUN_ROOT/search-turn.out"
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

workspace="$RUN_ROOT/turn-integrity-workspace"
mkdir -p "$workspace"
git init -q -b main "$workspace"
git -C "$workspace" config user.name aimee-e2e
git -C "$workspace" config user.email aimee-e2e@example.invalid
printf 'turn integrity live workspace\n' >"$workspace/README.md"
git -C "$workspace" add README.md
git -C "$workspace" commit -qm initial
work_sid=ti-live-work
run_turn "$work_sid" TI_WRITE_FILE "$workspace" "$RUN_ROOT/write-turn.out"
session_workspace="$(find "$workspace/.aimee/worktrees" -mindepth 2 -maxdepth 2 \
  -type d -name main -print -quit 2>/dev/null || true)"
[[ -n "$session_workspace" ]] || fail "write turn did not create a managed session workspace"
[[ "$(<"$session_workspace/turn-integrity-live.txt")" == "live-contract-ok" ]] || \
  fail "write_file did not produce the expected bytes"
ok "model-backed write_file executed with exact readback"

run_turn "$work_sid" TI_EDIT_FILE "$workspace" "$RUN_ROOT/edit-turn.out"
[[ "$(<"$session_workspace/turn-integrity-live.txt")" == "live-contract-edited" ]] || \
  fail "edit_file did not produce the expected bytes"
ok "model-backed edit_file executed with exact readback"

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
git --git-dir="$git_bare" symbolic-ref HEAD refs/heads/main
git -C "$git_work" push -qu origin main
git -C "$git_work" remote set-head origin main
run_turn ti-live-push TI_GIT_PUSH "$git_work" "$RUN_ROOT/push-turn.out"
session_refs="$(git --git-dir="$git_bare" for-each-ref --format='%(refname)' refs/heads \
  | grep -v '^refs/heads/main$' || true)"
[[ -n "$session_refs" ]] || fail "authorized git_push did not publish the session branch"
ok "authorized external git_push reached a disposable real remote"

# A real stdio MCP peer receives this non-idempotent external mutation and then
# withholds its acknowledgement. Drive the production dispatcher through its
# first-class trusted-local API: the primary code role intentionally cannot call
# remote MCP tools, and accepting its policy refusal here used to be a false
# positive. The response must prove the peer was reached and timed out; the WORM
# assertion below independently proves that the timeout became unknown_outcome.
python3 - "$uds" <<'PY' >"$RUN_ROOT/mcp-timeout-tool.out"
import json
import socket
import sys

sock_path = sys.argv[1]
body = json.dumps({
    "method": "tool.execute",
    "tool": "ti_remote:mutate",
    "arguments": json.dumps({"request": "turn-integrity-live-mutation"}),
    "session_id": "ti-live-unknown",
    "timeout_ms": 1000,
}, separators=(",", ":"))
request = (
    "POST /v1/tools/execute HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    f"Content-Length: {len(body.encode())}\r\n\r\n{body}"
).encode()
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(15)
sock.connect(sock_path)
sock.sendall(request)
chunks = []
while True:
    data = sock.recv(65536)
    if not data:
        break
    chunks.append(data)
response = b"".join(chunks)
_, _, payload = response.partition(b"\r\n\r\n")
sys.stdout.buffer.write(payload)
PY
grep -q 'remote mcp tool failed: transport timeout' "$RUN_ROOT/mcp-timeout-tool.out" || {
  sed -n '1,20p' "$RUN_ROOT/mcp-timeout-tool.out" >&2
  fail "external MCP mutation did not reach the production timeout boundary"
}
ok "non-idempotent external MCP timeout completed as an uncertain outcome"

# Observe one session through the canonical OpenAI ingress, invalidate knowledge
# through the public API, then observe the same session again. Supplying a
# read-only tool keeps the request on the neutral IR path; the second provider
# request must carry the typed stale marker.
fresh_sid=ti-live-freshness
run_ir_turn() {
  local prompt="$1" out="$2"
  curl -fksS --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    -H "Authorization: Bearer $BEARER" -H "aimee-session-id: $fresh_sid" \
    -H 'content-type: application/json' -X POST \
    -d "{\"model\":\"turn-integrity-fixture\",\"messages\":[{\"role\":\"user\",\"content\":\"$prompt\"}],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"read a file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}}]}" \
    "$SERVER_URL/v1/chat/completions" >"$out"
  grep -q 'TURN_INTEGRITY_FIXTURE_DONE' "$out" || fail "canonical IR turn failed"
}
run_ir_turn TI_FRESHNESS_BEFORE "$RUN_ROOT/fresh-before.out"
curl -fksS --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
  -H "Authorization: Bearer $BEARER" -H 'content-type: application/json' -X POST \
  -d '{"source_kind":"repository","source_id":"turn-integrity-e2e","artifacts_stale":1}' \
  "$SERVER_URL/v1/curator/invalidated" >/dev/null
run_ir_turn TI_FRESHNESS_AFTER "$RUN_ROOT/fresh-after.out"
grep -q 'aimee-freshness' "$provider_log" || \
  fail "knowledge invalidation did not add the freshness marker"
ok "live invalidation produced a stale-knowledge instruction on the next turn"

# Run and store a real evaluation against the registered fixture model.
AIMEE_HOME="$client_home" "$REPO/aimee" eval run \
  "$REPO/scripts/fixtures/turn-integrity-eval" --seed 4242 >/dev/null
eval_json="$(AIMEE_HOME="$client_home" "$REPO/aimee" --json eval results \
  turn-integrity-eval)"
[[ "$eval_json" == *'turn integrity live fixture'* && "$eval_json" == *'"success":true'* ]] || \
  fail "stored eval row missing"
# The live-stack harness may isolate DB1 with its application-level
# `search_path` URL parameter. libpq does not recognize that parameter, so
# translate it to PGOPTIONS when querying the same schema with psql.
store_url="${AIMEE_STORE_URL:?}"
store_search_path="$(sed -nE 's/.*[?&]search_path=([^&]*).*/\1/p' <<<"$store_url")"
psql_store_url="$(sed -E \
  -e 's/([?&])search_path=[^&]*(&|$)/\1/' \
  -e 's/\?&/?/' \
  -e 's/[?&]$//' <<<"$store_url")"
psql_env=()
if [[ -n "$store_search_path" ]]; then
  [[ "$store_search_path" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || \
    fail "invalid store search_path for psql"
  psql_env=(env "PGOPTIONS=-c search_path=$store_search_path")
fi
manifest_row="$("${psql_env[@]}" psql "$psql_store_url" -AtF '|' -c \
  "select dataset_hash,target_hash,harness_version,hardware_profile,seed from eval_results where suite='turn-integrity-eval' order by id desc limit 1")"
IFS='|' read -r dataset_hash target_hash harness_version hardware_profile eval_seed \
  <<<"$manifest_row"
[[ "$dataset_hash" =~ ^[0-9a-f]{64}$ && "$target_hash" =~ ^[0-9a-f]{64}$ ]] || \
  fail "stored eval row has no immutable dataset/target identity"
[[ "$harness_version" == 2 && -n "$hardware_profile" && "$eval_seed" == 4242 ]] || \
  fail "stored eval manifest does not bind harness, hardware, and seed"
ok "live benchmark execution stored its immutable manifest identity"

python3 - "$SCRATCH/audit/worm-live.db" <<'PY' || fail "turn/effect audit evidence missing"
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
rows = db.execute("select action, detail from audit_event order by seq").fetchall()
actions = {row[0] for row in rows}
details = "\n".join(row[1] or "" for row in rows)
required = {
    "turn.created",
    "turn.contextualized",
    "turn.completed",
    "effect.proposed",
    "effect.validated",
    "effect.executing",
    "effect.postcondition",
    "effect.completed",
    "knowledge.invalidated",
}
if not required.issubset(actions):
    raise SystemExit(1)
if not any(action == "effect.completed" and "state=unknown_outcome" in (detail or "")
           for action, detail in rows):
    raise SystemExit(1)
if "live-contract-ok" in details or "turn-integrity-live.txt" in details:
    raise SystemExit(1)
PY
ok "WORM contains bounded turn/effect/freshness lifecycles without raw arguments"

# Freeze the real KB process, require a bounded typed failure with no external
# continuation, then resume and prove the server-to-KB path recovers.
kill -STOP "$KB_PID"
kb_stopped=1
run_turn ti-live-kb-down TI_SEARCH_KB_DOWN "$workspace" "$RUN_ROOT/kb-down-turn.out" 150
python3 - "$provider_log" <<'PY' || fail "KB transport failure was not typed or offered an external continuation"
import json
import sys

for line in open(sys.argv[1], encoding="utf-8"):
    request = json.loads(line)
    body = request.get("body", {})
    wire = json.dumps(body, separators=(",", ":"))
    if "TI_SEARCH_KB_DOWN" not in wire:
        continue
    tool_wire = json.dumps(
        [m for m in body.get("messages", []) if m.get("role") == "tool"],
        separators=(",", ":"),
    )
    if 'status\\\":\\\"failed' in tool_wire and 'continuations\\\":[]' in tool_wire:
        if "web_search" in tool_wire or 'action\\\":' in tool_wire:
            raise SystemExit(1)
        raise SystemExit(0)
raise SystemExit(1)
PY
kill -CONT "$KB_PID"
kb_stopped=0
for _ in $(seq 1 50); do
  if curl -fksS --cert "$CLIENT_CERT" --key "$CLIENT_KEY" \
    -H "Authorization: Bearer $BEARER" -H 'content-type: application/json' -X POST \
    -d '{"query":"turn-integrity-recovery","scope":"all","max_results":1}' \
    "$SERVER_URL/v1/kb/search" | grep -q '"hits"'; then
    break
  fi
  sleep 0.2
done
run_turn ti-live-kb-recovered TI_SEARCH_EMPTY "$workspace" "$RUN_ROOT/kb-recovered-turn.out"
ok "KB transport failure was typed and bounded; live server→KB retrieval recovered"

echo "turn-integrity-live-e2e: $pass checks passed"
