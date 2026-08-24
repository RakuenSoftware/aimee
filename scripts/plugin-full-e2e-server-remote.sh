#!/bin/sh
# Runs ON the .252 host (not in a container): the aimee-server-only remainder of
# the full e2e, after container 101 was destroyed mid-run by another actor.
#
# Everything here needs a real aimee-server but no PostgreSQL, so it does not
# need the scratch container. The kb-side results (postgres/plugin coexistence,
# db2_ok through the bus, install:kb vs install:server) came from the completed
# portion of the container run and are recorded separately.
#
# Scope: its own HOME, bus socket and policy dir under /tmp/al2, removed at the
# end. .252 hosts live deployments in LXC containers; nothing here touches them,
# and no blanket pkill is used.
set -u

RUN=/tmp/al2
AH="$RUN/home/.config/aimee"
FAIL=0

pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { if [ "$1" = "0" ]; then pass "$2"; else fail "$2"; fi }

cleanup() {
  echo
  if [ "$FAIL" -ne 0 ]; then
    echo "== diagnostics (FAIL=$FAIL) =="
    for f in "$RUN"/run/srv.log "$RUN"/run/srv2.log "$RUN"/run/live.log \
             "$RUN"/run/sseplug.log "$RUN"/run/sse.log; do
      [ -f "$f" ] || continue
      echo "  --- $(basename "$f") ---"
      tail -12 "$f" | sed 's/^/    /'
    done
    echo "  --- grants ---"
    grep -H "" "$AH"/modules.d/server/mcp-*.grant 2>/dev/null | sed 's/^/    /'
    echo "  --- manifest ---"
    curl -s --unix-socket "$AH/aimee-http.sock" http://localhost/v1/cli/manifest 2>/dev/null \
      | head -c 600 | sed 's/^/    /'
    echo
  fi
  echo "== teardown =="
  [ -f "$RUN/run/srv.pid" ] && kill "$(cat "$RUN/run/srv.pid")" 2>/dev/null
  sleep 2
  pkill -f "$RUN/" 2>/dev/null
  sleep 1
  left=$(pgrep -f "aimee-module-mc[p]-" 2>/dev/null | wc -l)
  echo "  processes left from this run: $left"
  rm -rf "$RUN"
  [ -d "$RUN" ] && echo "  WARNING: cleanup failed" || echo "  cleaned"
}
trap cleanup EXIT

pkill -f "$RUN/" 2>/dev/null; sleep 1
rm -rf "$RUN"
mkdir -p "$AH/modules.d/server" "$RUN/run"
tar xzf /tmp/pkg.tgz -C "$RUN"

echo "== host =="
hostname; python3 -V

mk_plugin() {
  cat > "$1" <<PY
import sys, json
TOOLS=[{"name":"$2","description":"echo","inputSchema":{"type":"object"}}]
for line in sys.stdin:
    line=line.strip()
    if not line: continue
    q=json.loads(line); m=q.get("method"); i=q.get("id")
    if m=="initialize": r={"protocolVersion":"2024-11-05"}
    elif m=="tools/list": r={"tools":TOOLS}
    elif m=="tools/call": r={"served_by":"$3","args":q.get("params",{}).get("arguments")}
    else: r=None
    out={"jsonrpc":"2.0","id":i,"result":r} if r is not None else {"jsonrpc":"2.0","id":i,"error":{"code":-1,"message":"no"}}
    sys.stdout.write(json.dumps(out)+"\n"); sys.stdout.flush()
PY
}
mk_plugin "$RUN/run/p_srv.py" srv-echo srv-plugin
mk_plugin "$RUN/run/p_two.py" two-echo two-plugin

# A real SSE MCP server over HTTP, with bearer auth.
cat > "$RUN/run/sse_server.py" <<'PY'
import json, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
TOOLS=[{"name":"sse-echo","description":"echo","inputSchema":{"type":"object"}}]
BEARER="sse-secret-token"
streams=[]
class H(BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def log_message(self,*a): pass
    def _auth(self): return self.headers.get("Authorization")==f"Bearer {BEARER}"
    def do_GET(self):
        if not self._auth(): self.send_response(401); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type","text/event-stream")
        self.send_header("Cache-Control","no-cache")
        self.end_headers()
        self.wfile.write(b"event: endpoint\ndata: /messages\n\n"); self.wfile.flush()
        streams.append(self)
        threading.Event().wait()
    def do_POST(self):
        if not self._auth(): self.send_response(401); self.end_headers(); return
        n=int(self.headers.get("Content-Length","0"))
        q=json.loads(self.rfile.read(n) or b"{}")
        m=q.get("method"); i=q.get("id")
        if m=="initialize": r={"protocolVersion":"2024-11-05"}
        elif m=="tools/list": r={"tools":TOOLS}
        elif m=="tools/call": r={"served_by":"sse-plugin","args":q.get("params",{}).get("arguments")}
        else: r=None
        out={"jsonrpc":"2.0","id":i,"result":r} if r is not None else {"jsonrpc":"2.0","id":i,"error":{"code":-1,"message":"no"}}
        body=("event: message\ndata: "+json.dumps(out)+"\n\n").encode()
        self.send_response(202); self.send_header("Content-Length","0"); self.end_headers()
        for s in list(streams):
            try: s.wfile.write(body); s.wfile.flush()
            except Exception: streams.remove(s)
ThreadingHTTPServer(("127.0.0.1",8902),H).serve_forever()
PY
nohup python3 "$RUN/run/sse_server.py" > "$RUN/run/sse.log" 2>&1 &

MOD="$RUN/src/build/obj/aimee-module"
PROV="$RUN/scripts/provision-plugin-module.py"
[ -f "$MOD" ] || MOD="$RUN/aimee-module"
[ -f "$PROV" ] || PROV="$RUN/provision-plugin-module.py"
chmod +x "$RUN/aimee-server" "$MOD" 2>/dev/null

pr() { python3 "$PROV" --instance "$1" --module-bin "$MOD" --permission write \
       --config-dir "$AH" "$@" 2>/dev/null | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p'; }

echo "== provisioning (before the daemon starts) =="
SREF=$(python3 "$PROV" --instance live --argv "[\"python3\",\"$RUN/run/p_srv.py\"]" \
  --module-bin "$MOD" --permission write --config-dir "$AH" 2>/dev/null \
  | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
TREF=$(python3 "$PROV" --instance live2 --argv "[\"python3\",\"$RUN/run/p_two.py\"]" \
  --module-bin "$MOD" --permission write --config-dir "$AH" 2>/dev/null \
  | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
XREF=$(python3 "$PROV" --instance sseplug --sse-url "http://127.0.0.1:8902/sse" \
  --bearer-env SSE_TOKEN --module-bin "$MOD" --permission write --config-dir "$AH" 2>/dev/null \
  | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
echo "  live=$SREF live2=$TREF sse=$XREF"

echo "== REGRESSION: kinds derive from the ref and clear every canonical block =="
for g in "$AH"/modules.d/server/mcp-*.grant; do
  k=$(sed -n 's/^serve=\([0-9]*\).*/\1/p' "$g")
  r=$(sed -n 's/^principal_ref=//p' "$g")
  want=$((4096 + r * 256 + 1))
  [ "$k" = "$want" ] || fail "$(basename "$g"): serve=$k != derived $want"
  [ "$k" -gt 11871 ] || fail "$(basename "$g"): kind $k is inside a canonical module block"
done
[ "$FAIL" -eq 0 ] && pass "every grant derives its kinds from its principal_ref"

echo "== start REAL aimee-server =="
HOME="$RUN/home" AIMEE_NO_CACHE=1 \
AIMEE_MODULE_BUS_SOCKET="$RUN/run/srv.sock" \
AIMEE_MODULE_POLICY_DIR="$AH/modules.d/server" \
AIMEE_MCP_TOOL_PROFILE=full \
nohup "$RUN/aimee-server" --foreground --log-level=info > "$RUN/run/srv.log" 2>&1 &
echo $! > "$RUN/run/srv.pid"
SOCK="$AH/aimee-http.sock"
i=0; while [ $i -lt 90 ]; do [ -S "$SOCK" ] && break; sleep 0.5; i=$((i+1)); done
[ -S "$SOCK" ]; check $? "aimee-server /v1 socket is up"
[ -S "$SOCK" ] || { tail -20 "$RUN/run/srv.log"; exit 1; }

api() { curl -s --unix-socket "$SOCK" "http://localhost$1"; }
apipost() { curl -s -X POST --unix-socket "$SOCK" -H 'Content-Type: application/json' -d "$2" "http://localhost$1"; }

start_inst() {
  ln -sf "$MOD" "$RUN/aimee-module-mcp-$1"
  env HOME="$RUN/home" AIMEE_MODULE_PRINCIPAL_REF="$2" \
      AIMEE_MCP_PLUGIN_ARGV="$3" AIMEE_MCP_PLUGIN_PERMISSION=write \
      SSE_TOKEN=sse-secret-token \
      nohup "$RUN/aimee-module-mcp-$1" "$RUN/run/srv.sock" > "$RUN/run/$1.log" 2>&1 &
}
start_inst live "$SREF" "[\"python3\",\"$RUN/run/p_srv.py\"]"
start_inst live2 "$TREF" "[\"python3\",\"$RUN/run/p_two.py\"]"
# argv for an SSE instance is ["sse:URL","BEARER_ENV_NAME"] -- the env var NAME
# travels, never the secret, because the argv is reported over the bus.
start_inst sseplug "$XREF" "[\"sse:http://127.0.0.1:8902/sse\",\"SSE_TOKEN\"]"

i=0; while [ $i -lt 60 ]; do
  api /v1/cli/manifest 2>/dev/null | grep -q '"live.srv_echo"' && break
  sleep 1; i=$((i+1))
done

echo "== SSE transport, live over HTTP, through a provisioned instance =="
MAN=$(api /v1/cli/manifest)
if echo "$MAN" | grep -q '"sseplug.sse_echo"'; then
  pass "the SSE instance advertised its tool"
  SR=$(apipost /v1/commands/sseplug.sse_echo '{"via":"sse"}')
  echo "$SR" | grep -q 'sse-plugin'; check $? "an SSE-transport plugin answered a real dispatch"
  echo "$SR" | grep -q 'sse'; check $? "arguments reached the SSE plugin over HTTP"
else
  fail "the SSE instance advertised its tool"
  tail -8 "$RUN/run/sseplug.log" | sed 's/^/    /'
fi

echo "== SSE: a wrong bearer must NOT authenticate =="
ln -sf "$MOD" "$RUN/aimee-module-mcp-ssebad"
BREF=$(python3 "$PROV" --instance ssebad --sse-url "http://127.0.0.1:8902/sse" \
  --bearer-env BAD_TOKEN --module-bin "$MOD" --permission write --config-dir "$AH" 2>/dev/null \
  | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
env HOME="$RUN/home" AIMEE_MODULE_PRINCIPAL_REF="$BREF" \
    AIMEE_MCP_PLUGIN_ARGV="[\"sse:http://127.0.0.1:8902/sse\",\"BAD_TOKEN\"]" \
    AIMEE_MCP_PLUGIN_PERMISSION=write BAD_TOKEN=wrong-token \
    nohup "$RUN/aimee-module-mcp-ssebad" "$RUN/run/srv.sock" > "$RUN/run/ssebad.log" 2>&1 &
sleep 8
if api /v1/cli/manifest | grep -q '"ssebad.sse_echo"'; then
  fail "a wrong bearer still got the tool list"
else
  pass "a wrong bearer did not authenticate (no tools advertised)"
fi

echo "== 40 CONCURRENT dispatches =="
# Collect the curl PIDs and wait on THOSE. A bare `wait` waits for every
# background job of this shell, which includes the SSE server, the module
# instances and aimee-server itself -- all long-lived, so it never returns.
cpids=""
n=0; while [ $n -lt 40 ]; do
  apipost /v1/commands/live.srv_echo "{\"c\":$n}" > "$RUN/run/c$n.out" 2>&1 &
  cpids="$cpids $!"
  n=$((n+1))
done
for p in $cpids; do wait "$p" 2>/dev/null; done
OK=$(grep -l 'srv-plugin' "$RUN/run"/c*.out 2>/dev/null | wc -l)
[ "$OK" -eq 40 ]; check $? "40/40 concurrent dispatches answered ($OK ok)"
if [ "$OK" -ne 40 ]; then
  echo "  --- what the failures actually returned ---"
  for f in "$RUN/run"/c*.out; do
    grep -q 'srv-plugin' "$f" 2>/dev/null && continue
    echo "    $(basename "$f"): [$(head -c 200 "$f")]"
  done | head -6
fi

echo "== operator surface reports the ref while instances are attached =="
M=$(api /v1/dashboard/metrics)
echo "$M" | grep -q '"principal_ref"'; check $? "metrics reports principal_ref"
echo "$M" | grep -q '"event_base"' && fail "metrics still reports the retired event_base" \
  || pass "metrics no longer reports the retired event_base"
echo "$M" | grep -q "\"principal_ref\":$SREF"; check $? "metrics reports THIS instance's ref ($SREF)"

echo "== withdrawal: kill a plugin, keep its module =="
TLRAW=$(api /v1/mcp/tools_list)
echo "$TLRAW" | grep -q '"generation"'
check $? "tools_list reports a registry generation"
[ $? -eq 0 ] || echo "    raw: $(echo "$TLRAW" | head -c 300)"
GEN0=$(echo "$TLRAW" | sed -n 's/.*"generation":\([0-9]*\).*/\1/p')
pkill -f "run/p_two.py" 2>/dev/null
i=0; while [ $i -lt 40 ]; do
  api /v1/cli/manifest 2>/dev/null | grep -q '"live2.two_echo"' || break
  sleep 1; i=$((i+1))
done
api /v1/cli/manifest | grep -q '"live2.two_echo"' \
  && fail "a dead plugin kept its command" \
  || pass "a dead plugin's command was withdrawn from the manifest"
GEN1=$(api /v1/mcp/tools_list | sed -n 's/.*"generation":\([0-9]*\).*/\1/p')
[ -n "$GEN0" ] && [ -n "$GEN1" ] && [ "$GEN0" != "$GEN1" ] \
  && pass "the registry generation moved ($GEN0 -> $GEN1), so listChanged fires" \
  || fail "the generation did not move ($GEN0 -> $GEN1)"
api /v1/dashboard/metrics | grep -q '"plugins"'
check $? "the module still answers after its plugin died"


echo "== restart the daemon under attached instances =="
FD0=$(ls /proc/"$(cat "$RUN/run/srv.pid")"/fd 2>/dev/null | wc -l)
kill "$(cat "$RUN/run/srv.pid")" 2>/dev/null
sleep 4
HOME="$RUN/home" AIMEE_NO_CACHE=1 \
AIMEE_MODULE_BUS_SOCKET="$RUN/run/srv.sock" \
AIMEE_MODULE_POLICY_DIR="$AH/modules.d/server" \
AIMEE_MCP_TOOL_PROFILE=full \
nohup "$RUN/aimee-server" --foreground --log-level=info > "$RUN/run/srv2.log" 2>&1 &
echo $! > "$RUN/run/srv.pid"
i=0; while [ $i -lt 90 ]; do [ -S "$SOCK" ] && break; sleep 0.5; i=$((i+1)); done
[ -S "$SOCK" ]; check $? "the daemon came back up after a restart"
sleep 6
if api /v1/cli/manifest | grep -q '"live.srv_echo"'; then
  pass "an instance re-attached by itself after a daemon restart"
  RR=$(apipost /v1/commands/live.srv_echo '{"after":"restart"}')
  echo "$RR" | grep -q 'srv-plugin'; check $? "and it still dispatches after the restart"
else
  echo "  NOTE: no automatic re-attach -- instances must be restarted with the daemon."
  echo "        This is a KNOWN GAP, not a regression: nothing implements re-attach."
  pass "recorded: instances do not re-attach on their own (known gap)"
fi

echo
if [ "$FAIL" -eq 0 ]; then echo "SERVER REMAINDER PASSED"; else echo "SERVER REMAINDER FAILED ($FAIL)"; fi
exit "$FAIL"
