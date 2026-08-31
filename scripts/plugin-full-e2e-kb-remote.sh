#!/bin/sh
# FULL end-to-end, inside container 101 on 192.168.1.252.
#
# What makes this different from scripts/live-plugin-validation-remote.sh: that
# one stands up a real aimee-server on a scratch host with no other modules. This
# one runs a real aimee-kb against a real PostgreSQL with the real postgres
# module attached, AND a real aimee-server, on the same box at the same time --
# which is the configuration that exposed the event-kind collision.
#
# Scope: everything lives under /opt/plive and the scratch database aimee_plive.
# Container 101 is a dedicated full-e2e box; the live deployment on this Proxmox
# host runs in a DIFFERENT container (9078) and nothing here touches it.
set -u

RUN=/opt/plive
AH="$RUN/home/.config/aimee"
DSN="postgresql://plive:plive_e2e_pw@127.0.0.1:5432/aimee_plive"
KBPORT=8899
FAIL=0

pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { if [ "$1" = "0" ]; then pass "$2"; else fail "$2"; fi }

cleanup() {
  echo
  echo "== teardown =="
  for p in "$RUN/run/kb.pid" "$RUN/run/srv.pid"; do
    [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null
  done
  sleep 2
  pkill -f "$RUN/" 2>/dev/null
  sleep 1
  left=$(pgrep -f "aimee-module-mc[p]-" 2>/dev/null | wc -l)
  echo "  processes left from this run: $left"
}
trap cleanup EXIT

# Start from a clean slate but keep the unpacked binaries.
pkill -f "$RUN/" 2>/dev/null; sleep 2
rm -rf "$RUN/home" "$RUN/run"
mkdir -p "$AH/modules.d/kb" "$AH/modules.d/server" "$RUN/run"

echo "== host =="
hostname; python3 -V; echo "postgres: $(/usr/lib/postgresql/17/bin/psql --version)"

# ---------------------------------------------------------------- fixtures ---
mk_plugin() { # $1 file, $2 tool name, $3 identity
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
mk_plugin "$RUN/run/p_srv.py"  srv-echo  srv-plugin
mk_plugin "$RUN/run/p_two.py"  two-echo  two-plugin
mk_plugin "$RUN/run/p_kbc.py"  kbc-echo  kbclient-plugin
mk_plugin "$RUN/run/p_svc.py"  svc-echo  srvclient-plugin

# A plugin that HANGS on tools/call: the invoke deadline must contain it.
cat > "$RUN/run/p_hang.py" <<'PY'
import sys, json, time
TOOLS=[{"name":"hang","description":"never answers","inputSchema":{"type":"object"}}]
for line in sys.stdin:
    line=line.strip()
    if not line: continue
    q=json.loads(line); m=q.get("method"); i=q.get("id")
    if m=="initialize": r={"protocolVersion":"2024-11-05"}
    elif m=="tools/list": r={"tools":TOOLS}
    elif m=="tools/call":
        time.sleep(3600)
        continue
    else: r=None
    out={"jsonrpc":"2.0","id":i,"result":r} if r is not None else {"jsonrpc":"2.0","id":i,"error":{"code":-1,"message":"no"}}
    sys.stdout.write(json.dumps(out)+"\n"); sys.stdout.flush()
PY

# A real SSE MCP server, so the SSE transport is exercised against HTTP rather
# than only an in-process stub.
cat > "$RUN/run/sse_server.py" <<'PY'
import json, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
TOOLS=[{"name":"sse-echo","description":"echo","inputSchema":{"type":"object"}}]
BEARER="sse-secret-token"
streams=[]
class H(BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def log_message(self,*a): pass
    def _auth(self):
        return self.headers.get("Authorization")==f"Bearer {BEARER}"
    def do_GET(self):
        if not self._auth(): self.send_response(401); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type","text/event-stream")
        self.send_header("Cache-Control","no-cache")
        self.end_headers()
        self.wfile.write(b"event: endpoint\ndata: /messages\n\n"); self.wfile.flush()
        streams.append(self)
        ev=threading.Event()
        self.closed=ev
        ev.wait()
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
ThreadingHTTPServer(("127.0.0.1",8901),H).serve_forever()
PY
nohup python3 "$RUN/run/sse_server.py" > "$RUN/run/sse.log" 2>&1 &
echo $! > "$RUN/run/sse.pid"

PGARGV="[\"python3\",\"$RUN/aimee-pluggy-host.py\",\"--project\",\"aimee_demo\",\"--spec-module\",\"aimee_demo_spec\",\"--plugin-module\",\"aimee_demo_plugin\"]"

# ------------------------------------------------------------------ config ---
# One mcp_clients entry per daemon, with DIFFERENT plugins, so "kb booted only
# its own" is a sharp assertion rather than a process count.
cat > "$AH/aimee.yaml" <<YAML
db2_url: $DSN
mcp_clients:
  - name: kbclient
    transport: stdio
    install: kb
    command:
      - python3
      - $RUN/run/p_kbc.py
  - name: srvclient
    transport: stdio
    command:
      - python3
      - $RUN/run/p_svc.py
YAML

# ------------------------------------------------------- provision (KB side) --
# The postgres module's REAL grant, verbatim from the shipped bundle. This is
# what the old plugin range collided with.
ln -sf "$RUN/aimee-module" "$RUN/aimee-module-postgres"
cat > "$AH/modules.d/kb/postgres.grant" <<GRANT
version=1
principal_class=1
principal_ref=28
uid=self
executable=$RUN/aimee-module-postgres
publish=
subscribe=
request=
serve=11265
GRANT

prov() { # $1 instance, $2.. extra args; echoes the ref
  out=$(python3 "$RUN/provision-plugin-module.py" --instance "$1" \
        --module-bin "$RUN/aimee-module" --permission write \
        --config-dir "$AH" "$@" 2>&1) || { echo "PROVISION FAILED: $out" >&2; return 1; }
  echo "$out" | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p'
}

echo
echo "== provisioning (all BEFORE the daemons start: grants load once at boot) =="
KREF=$(python3 "$RUN/provision-plugin-module.py" --instance kbplug --daemon kb \
  --argv "[\"python3\",\"$RUN/run/p_srv.py\"]" --module-bin "$RUN/aimee-module" \
  --permission write --config-dir "$AH" 2>/dev/null | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
SREF=$(python3 "$RUN/provision-plugin-module.py" --instance live \
  --argv "[\"python3\",\"$RUN/run/p_srv.py\"]" --module-bin "$RUN/aimee-module" \
  --permission write --config-dir "$AH" 2>/dev/null | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
TREF=$(python3 "$RUN/provision-plugin-module.py" --instance live2 \
  --argv "[\"python3\",\"$RUN/run/p_two.py\"]" --module-bin "$RUN/aimee-module" \
  --permission write --config-dir "$AH" 2>/dev/null | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
HREF=$(python3 "$RUN/provision-plugin-module.py" --instance hangy \
  --argv "[\"python3\",\"$RUN/run/p_hang.py\"]" --module-bin "$RUN/aimee-module" \
  --permission write --config-dir "$AH" 2>/dev/null | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
XREF=$(python3 "$RUN/provision-plugin-module.py" --instance sseplug \
  --sse-url "http://127.0.0.1:8901/sse" --bearer-env SSE_TOKEN \
  --module-bin "$RUN/aimee-module" --permission write --config-dir "$AH" 2>/dev/null \
  | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
GREF=""
if [ -d "$RUN/pluggylib" ]; then
  GREF=$(python3 "$RUN/provision-plugin-module.py" --instance livepg --argv "$PGARGV" \
    --module-bin "$RUN/aimee-module" --permission write --config-dir "$AH" 2>/dev/null \
    | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
fi
echo "  kb=$KREF server: live=$SREF live2=$TREF hangy=$HREF sse=$XREF pluggy=${GREF:-none}"

echo
echo "== REGRESSION: plugin kinds must clear every canonical module block =="
# db1 holds the highest canonical ref (30); its block ends at 4096+30*256+255.
for g in "$AH"/modules.d/*/mcp-*.grant; do
  [ -f "$g" ] || continue
  k=$(sed -n 's/^serve=\([0-9]*\).*/\1/p' "$g")
  r=$(sed -n 's/^principal_ref=//p' "$g")
  want=$((4096 + r * 256 + 1))
  if [ "$k" != "$want" ]; then
    fail "$(basename "$g"): serve=$k is not the ref-derived kind $want"
  elif [ "$k" -le 11871 ]; then
    fail "$(basename "$g"): kind $k lands inside a canonical module block"
  fi
done
[ "$FAIL" -eq 0 ] && pass "every plugin grant derives its kinds from its principal_ref"
grep -q "^serve=11265$" "$AH/modules.d/kb/postgres.grant" && \
  pass "postgres still owns 11265 (the kind the old range squatted)" || \
  fail "postgres grant is not intact"

# ------------------------------------------------------------- start aimee-kb --
echo
echo "== start REAL aimee-kb (real postgres, real db2 schema) =="
HOME="$RUN/home" AIMEE_NO_CACHE=1 \
AIMEE_MODULE_BUS_SOCKET="$RUN/run/kb.sock" \
AIMEE_MODULE_POLICY_DIR="$AH/modules.d/kb" \
nohup "$RUN/aimee-kb" --http-port=$KBPORT > "$RUN/run/kb.log" 2>&1 &
echo $! > "$RUN/run/kb.pid"
i=0; while [ $i -lt 120 ]; do curl -sf "http://127.0.0.1:$KBPORT/v1/health" >/dev/null 2>&1 && break; sleep 1; i=$((i+1)); done
curl -sf "http://127.0.0.1:$KBPORT/v1/health" >/dev/null 2>&1
check $? "aimee-kb came up against a real PostgreSQL (${i}s)"

echo "== attach the REAL postgres module to kb =="
HOME="$RUN/home" AIMEE_DB2_URL="$DSN" AIMEE_MODULE_PRINCIPAL_REF=28 \
nohup "$RUN/aimee-module-postgres" "$RUN/run/kb.sock" > "$RUN/run/pg.log" 2>&1 &
sleep 5
HEALTH=$(curl -s "http://127.0.0.1:$KBPORT/v1/health")
echo "$HEALTH" | grep -q '"db2_ok":true'
check $? "kb reports db2_ok=true THROUGH the postgres module on the bus"

echo "== attach a plugin instance to the SAME kb bus =="
ln -sf "$RUN/aimee-module" "$RUN/aimee-module-mcp-kbplug"
HOME="$RUN/home" AIMEE_MODULE_PRINCIPAL_REF="$KREF" \
AIMEE_MCP_PLUGIN_ARGV="[\"python3\",\"$RUN/run/p_srv.py\"]" \
AIMEE_MCP_PLUGIN_PERMISSION=write \
nohup "$RUN/aimee-module-mcp-kbplug" "$RUN/run/kb.sock" > "$RUN/run/kbplug.log" 2>&1 &
sleep 5
grep -qi "attach denied" "$RUN/run/kbplug.log"
if [ $? -eq 0 ]; then
  fail "the plugin instance was denied at attach on kb"
  sed 's/^/    /' "$RUN/run/kbplug.log" | tail -5
else
  pass "a plugin instance and the postgres module COEXIST on one kb bus"
fi
HEALTH2=$(curl -s "http://127.0.0.1:$KBPORT/v1/health")
echo "$HEALTH2" | grep -q '"db2_ok":true'
check $? "postgres was NOT displaced: db2_ok is still true"

echo "== kb booted only ITS OWN mcp_clients =="
pgrep -f "run/p_kbc.py" >/dev/null 2>&1
check $? "kb started the install:kb client (kbclient)"
if pgrep -f "run/p_svc.py" >/dev/null 2>&1; then
  fail "kb also started the install:server client (srvclient)"
else
  pass "kb did NOT start the install:server client"
fi

# --------------------------------------------------------- start aimee-server --
echo
echo "== start REAL aimee-server on the same box =="
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
apipost_t() { curl -s --max-time "$3" -X POST --unix-socket "$SOCK" -H 'Content-Type: application/json' -d "$2" "http://localhost$1"; }

start_inst() { # $1 instance, $2 ref, $3 argv-json, $4 extra env assignments
  ln -sf "$RUN/aimee-module" "$RUN/aimee-module-mcp-$1"
  env HOME="$RUN/home" AIMEE_MODULE_PRINCIPAL_REF="$2" \
      AIMEE_MCP_PLUGIN_ARGV="$3" AIMEE_MCP_PLUGIN_PERMISSION=write \
      PYTHONPATH="$RUN/pluggylib:$RUN/fixtures" SSE_TOKEN=sse-secret-token \
      nohup "$RUN/aimee-module-mcp-$1" "$RUN/run/srv.sock" > "$RUN/run/$1.log" 2>&1 &
}

echo "== start the plugin instances =="
start_inst live  "$SREF" "[\"python3\",\"$RUN/run/p_srv.py\"]"
start_inst live2 "$TREF" "[\"python3\",\"$RUN/run/p_two.py\"]"
start_inst hangy "$HREF" "[\"python3\",\"$RUN/run/p_hang.py\"]"
# argv for an SSE instance is ["sse:URL", "BEARER_ENV_NAME"] -- the env var
# NAME travels, never the secret, because the argv is reported over the bus.
start_inst sseplug "$XREF" "[\"sse:http://127.0.0.1:8901/sse\",\"SSE_TOKEN\"]"
[ -n "$GREF" ] && start_inst livepg "$GREF" "$PGARGV"

i=0
while [ $i -lt 60 ]; do
  api /v1/cli/manifest 2>/dev/null | grep -q '"live.srv_echo"' && break
  sleep 1; i=$((i+1))
done

echo
echo "== the command list =="
MAN=$(api /v1/cli/manifest)
echo "$MAN" | grep -q '"live.srv_echo"'; check $? "manifest advertises live.srv_echo"
echo "$MAN" | grep -q '"live2.two_echo"'; check $? "manifest advertises live2.two_echo"
echo "$MAN" | grep -q '"/v1/commands/live.srv_echo"'; check $? "manifest carries the FULL invoke path"

echo "== dispatch: HTTP ingress -> EVENT BUS -> plugin =="
R=$(apipost /v1/commands/live.srv_echo '{"hello":"world"}')
echo "$R" | grep -q 'srv-plugin'; check $? "the call crossed the bus to the real plugin"
echo "$R" | grep -q 'world'; check $? "arguments reached the plugin"
R2=$(apipost /v1/commands/live2.two_echo '{}')
echo "$R2" | grep -q 'two-plugin'; check $? "instance 2 answered from ITS plugin"

echo "== SSE transport, live over HTTP, through a provisioned instance =="
if echo "$MAN" | grep -q '"sseplug.sse_echo"'; then
  pass "the SSE instance advertised its tool"
  SR=$(apipost /v1/commands/sseplug.sse_echo '{"via":"sse"}')
  echo "$SR" | grep -q 'sse-plugin'; check $? "an SSE-transport plugin answered a real dispatch"
  echo "$SR" | grep -q 'sse'; check $? "arguments reached the SSE plugin"
else
  fail "the SSE instance advertised its tool"
  tail -6 "$RUN/run/sseplug.log" | sed 's/^/    /'
fi

echo "== pluggy over the bus =="
if [ -n "$GREF" ]; then
  if echo "$MAN" | grep -q '"livepg.greet"'; then
    pass "the pluggy instance advertised its hook as a command"
    PR=$(apipost /v1/commands/livepg.greet '{"name":"ada"}')
    echo "$PR" | grep -q 'hello ada'; check $? "a pluggy hook crossed the event bus to the real plugin"
  else
    fail "the pluggy instance advertised its hook"
    tail -6 "$RUN/run/livepg.log" | sed 's/^/    /'
  fi
fi

echo "== server booted only ITS OWN mcp_clients =="
TL=$(api /v1/mcp/tools_list)
echo "$TL" | grep -q 'srvclient:'; check $? "server booted the install:server client"
if echo "$TL" | grep -q 'kbclient:'; then
  fail "server also booted the install:kb client"
else
  pass "server did NOT boot the install:kb client"
fi

# ------------------------------------------------------------- adversarial ---
echo
echo "== exploratory: a plugin that never answers =="
T0=$(date +%s)
HR=$(apipost_t /v1/commands/hangy.hang '{}' 90)
T1=$(date +%s)
ELAPSED=$((T1-T0))
if [ "$ELAPSED" -lt 85 ]; then
  pass "a hanging plugin was bounded by the invoke deadline (${ELAPSED}s), not left to hang"
else
  fail "a hanging plugin was not bounded (${ELAPSED}s)"
fi
api /v1/cli/manifest | grep -q '"live.srv_echo"'
check $? "the hung instance did not take the other instances down"

echo "== exploratory: malformed and hostile inputs =="
apipost /v1/commands/live.nope '{}' | grep -qi 'no such command'
check $? "an unknown verb is refused, not dispatched"
apipost /v1/commands/nosuch.thing '{}' | grep -qi 'no such command'
check $? "an unknown GROUP is refused"
apipost /v1/commands/live.srv_echo 'not json at all' >/dev/null 2>&1
check $? "a non-JSON body does not crash the daemon"
BIG=$(python3 -c "print('{\"a\":\"' + 'x'*2000000 + '\"}')")
echo "$BIG" | curl -s --max-time 30 -X POST --unix-socket "$SOCK" \
  -H 'Content-Type: application/json' --data-binary @- \
  "http://localhost/v1/commands/live.srv_echo" >/dev/null 2>&1
check $? "a 2MB argument body is handled without killing the daemon"
api /v1/cli/manifest | grep -q '"live.srv_echo"'
check $? "the daemon still serves after the hostile inputs"

echo "== exploratory: an UNGRANTED instance is refused =="
ln -sf "$RUN/aimee-module" "$RUN/aimee-module-mcp-rogue"
env HOME="$RUN/home" AIMEE_MODULE_PRINCIPAL_REF=455 \
    AIMEE_MCP_PLUGIN_ARGV="[\"python3\",\"$RUN/run/p_srv.py\"]" \
    AIMEE_MCP_PLUGIN_PERMISSION=write \
    nohup "$RUN/aimee-module-mcp-rogue" "$RUN/run/srv.sock" > "$RUN/run/rogue.log" 2>&1 &
sleep 4
grep -qi "denied" "$RUN/run/rogue.log"
check $? "an instance with no grant is denied at attach"

echo "== exploratory: a STALE event base is refused (migration guard) =="
ln -sf "$RUN/aimee-module" "$RUN/aimee-module-mcp-stale"
env HOME="$RUN/home" AIMEE_MODULE_PRINCIPAL_REF="$SREF" \
    AIMEE_MODULE_EVENT_BASE=11264 \
    AIMEE_MCP_PLUGIN_ARGV="[\"python3\",\"$RUN/run/p_srv.py\"]" \
    AIMEE_MCP_PLUGIN_PERMISSION=write \
    nohup "$RUN/aimee-module-mcp-stale" "$RUN/run/srv.sock" > "$RUN/run/stale.log" 2>&1 &
sleep 4
grep -qi "stale" "$RUN/run/stale.log"
check $? "an instance carrying the retired AIMEE_MODULE_EVENT_BASE is refused with a fix-it message"

# -------------------------------------------------------------------- soak ---
echo
echo "== soak: 300 dispatches =="
RSS0=$(ps -o rss= -p "$(cat "$RUN/run/srv.pid")" 2>/dev/null | tr -d ' ')
FD0=$(ls /proc/"$(cat "$RUN/run/srv.pid")"/fd 2>/dev/null | wc -l)
BAD=0
n=0
while [ $n -lt 300 ]; do
  apipost /v1/commands/live.srv_echo "{\"n\":$n}" | grep -q 'srv-plugin' || BAD=$((BAD+1))
  n=$((n+1))
done
[ "$BAD" -eq 0 ]; check $? "300/300 dispatches answered correctly ($BAD bad)"
RSS1=$(ps -o rss= -p "$(cat "$RUN/run/srv.pid")" 2>/dev/null | tr -d ' ')
FD1=$(ls /proc/"$(cat "$RUN/run/srv.pid")"/fd 2>/dev/null | wc -l)
echo "  server RSS ${RSS0}K -> ${RSS1}K, fds ${FD0} -> ${FD1}"
GROW=$((FD1-FD0))
[ "$GROW" -le 8 ]; check $? "no file-descriptor leak across 300 dispatches (+$GROW)"

echo "== soak: 40 CONCURRENT dispatches =="
# Collect the curl PIDs and wait on THOSE. A bare `wait` waits for every
# background job of this shell -- which includes the SSE server, the module
# instances, aimee-kb and aimee-server -- so it would never return.
cpids=""
n=0; while [ $n -lt 40 ]; do
  apipost /v1/commands/live.srv_echo "{\"c\":$n}" > "$RUN/run/c$n.out" 2>&1 &
  cpids="$cpids $!"
  n=$((n+1))
done
for p in $cpids; do wait "$p" 2>/dev/null; done
OK=$(grep -l 'srv-plugin' "$RUN/run"/c*.out 2>/dev/null | wc -l)
[ "$OK" -eq 40 ]; check $? "40/40 concurrent dispatches answered ($OK ok)"

# ------------------------------------------------------- restart / re-attach --
echo
echo
echo "== withdrawal: kill a plugin, keep its module =="
GEN0=$(api /v1/mcp/tools_list | sed -n 's/.*"generation":\([0-9]*\).*/\1/p')
pkill -f "run/p_two.py" 2>/dev/null
i=0; while [ $i -lt 40 ]; do
  api /v1/cli/manifest 2>/dev/null | grep -q '"live2.two_echo"' || break
  sleep 1; i=$((i+1))
done
api /v1/cli/manifest | grep -q '"live2.two_echo"' && fail "a dead plugin kept its command" || pass "a dead plugin's command was withdrawn"
GEN1=$(api /v1/mcp/tools_list | sed -n 's/.*"generation":\([0-9]*\).*/\1/p')
[ -n "$GEN0" ] && [ -n "$GEN1" ] && [ "$GEN0" != "$GEN1" ] \
  && pass "the registry generation moved ($GEN0 -> $GEN1), so listChanged fires" \
  || fail "the generation did not move ($GEN0 -> $GEN1)"


echo "== restart the daemon under attached instances =="
kill "$(cat "$RUN/run/srv.pid")" 2>/dev/null
sleep 3
HOME="$RUN/home" AIMEE_NO_CACHE=1 \
AIMEE_MODULE_BUS_SOCKET="$RUN/run/srv.sock" \
AIMEE_MODULE_POLICY_DIR="$AH/modules.d/server" \
AIMEE_MCP_TOOL_PROFILE=full \
nohup "$RUN/aimee-server" --foreground --log-level=info > "$RUN/run/srv2.log" 2>&1 &
echo $! > "$RUN/run/srv.pid"
i=0; while [ $i -lt 90 ]; do [ -S "$SOCK" ] && break; sleep 0.5; i=$((i+1)); done
[ -S "$SOCK" ]; check $? "the daemon came back up"
sleep 5
if api /v1/cli/manifest | grep -q '"live.srv_echo"'; then
  pass "an instance re-attached by itself after a daemon restart"
else
  # Not a regression: nothing implements re-attach. Recorded, not failed, so a
  # green run means "no regressions", not "this gap was quietly accepted".
  echo "  NOTE: no automatic re-attach -- instances must be restarted with the daemon."
  pass "recorded: instances do not re-attach on their own (known gap)"
fi

echo "== kb is still healthy after all of it =="
curl -s "http://127.0.0.1:$KBPORT/v1/health" | grep -q '"db2_ok":true'
check $? "aimee-kb still reports db2_ok=true at the end of the run"

echo
if [ "$FAIL" -eq 0 ]; then echo "FULL E2E PASSED"; else echo "FULL E2E FAILED ($FAIL)"; fi
exit "$FAIL"
