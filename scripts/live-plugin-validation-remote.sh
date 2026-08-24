#!/bin/sh
# Runs ON the test host. See live-plugin-validation-252.sh for why.
set -u

RUN=/tmp/al
AH="$RUN/home/.config/aimee"
FAIL=0

pass() { echo "  ok: $1"; }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { if [ "$1" = "0" ]; then pass "$2"; else fail "$2"; fi }

cleanup() {
  echo "== teardown =="
  [ -f "$RUN/server.pid" ] && kill "$(cat "$RUN/server.pid")" 2>/dev/null
  sleep 1
  # Scoped to THIS run's directory only; .252 hosts a live deployment.
  pkill -f "$RUN" 2>/dev/null
  sleep 1
  # Exclude this shell: a pattern containing $RUN also matches the pgrep itself,
  # which reads as a leak that is not one.
  left=$(pgrep -f "aimee-module-mcp-live" 2>/dev/null | wc -l)
  echo "  processes left from this run: $left"
  rm -rf "$RUN" /tmp/live-pkg.tgz
  [ -d "$RUN" ] && echo "  WARNING: cleanup failed" || echo "  cleaned"
}
trap cleanup EXIT

rm -rf "$RUN"
mkdir -p "$RUN/home" "$AH/modules.d/server"
tar xzf /tmp/live-pkg.tgz -C "$RUN"

SRV="$RUN/aimee-server"
MOD="$RUN/src/build/obj/aimee-module"
PROV="$RUN/scripts/provision-plugin-module.py"
chmod +x "$SRV" "$MOD" 2>/dev/null

echo "== host =="
hostname; uname -m; python3 -V

# A real MCP server plugin. Its tool name is not registry-legal on purpose.
cat > "$RUN/plugin.py" <<'PY'
import sys, json
TOOLS=[{"name":"live-echo","description":"Echo back","inputSchema":{"type":"object"}}]
for line in sys.stdin:
    line=line.strip()
    if not line: continue
    q=json.loads(line); m=q.get("method"); i=q.get("id")
    if m=="initialize": r={"protocolVersion":"2024-11-05"}
    elif m=="tools/list": r={"tools":TOOLS}
    elif m=="tools/call": r={"served_by":"live-plugin","args":q.get("params",{}).get("arguments")}
    else: r=None
    out={"jsonrpc":"2.0","id":i,"result":r} if r is not None else {"jsonrpc":"2.0","id":i,"error":{"code":-1,"message":"no"}}
    sys.stdout.write(json.dumps(out)+"\n"); sys.stdout.flush()
PY

echo "== provision =="
ENV_OUT=$(python3 "$PROV" --instance live --argv "[\"python3\",\"$RUN/plugin.py\"]" \
  --module-bin "$MOD" --permission write --config-dir "$AH" 2>&1) || { echo "$ENV_OUT"; exit 1; }
REF=$(echo "$ENV_OUT" | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
BASE=$(echo "$ENV_OUT" | sed -n 's/^AIMEE_MODULE_EVENT_BASE=//p')
echo "  principal_ref=$REF event_base=$BASE"
[ -f "$AH/modules.d/server/mcp-live.grant" ] && pass "grant written" || fail "grant missing"

# Declare a config-based MCP client too, so the path being considered for
# retirement runs on the SAME server as its replacement.
cat > "$AH/aimee.yaml" <<YAML
mcp_clients:
  - name: cfgplugin
    transport: stdio
    command:
      - python3
      - $RUN/plugin.py
YAML


# All instances are provisioned BEFORE the server starts. bus_runtime_start()
# loads the grant policy dir ONCE; an instance provisioned afterwards is denied
# at attach until the daemon restarts. That is a real operational constraint,
# recorded in the validation notes.
ENV2=$(python3 "$PROV" --instance live2 --argv "[\"python3\",\"$RUN/plugin2.py\"]" \
  --module-bin "$MOD" --permission write --config-dir "$AH" 2>&1) || echo "$ENV2"
REF2=$(echo "$ENV2" | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
BASE2=$(echo "$ENV2" | sed -n 's/^AIMEE_MODULE_EVENT_BASE=//p')
sed 's/live-echo/live-two/; s/live-plugin/live-plugin-2/' "$RUN/plugin.py" > "$RUN/plugin2.py"

PGARGV=""
if [ -d "$RUN/pluggylib" ]; then
  PGARGV="[\"python3\",\"$RUN/scripts/aimee-pluggy-host.py\",\"--project\",\"aimee_demo\",\"--spec-module\",\"aimee_demo_spec\",\"--plugin-module\",\"aimee_demo_plugin\"]"
  ENV3=$(python3 "$PROV" --instance livepg --argv "$PGARGV" \
    --module-bin "$MOD" --permission write --config-dir "$AH" 2>&1) || echo "$ENV3"
  REF3=$(echo "$ENV3" | sed -n 's/^AIMEE_MODULE_PRINCIPAL_REF=//p')
  BASE3=$(echo "$ENV3" | sed -n 's/^AIMEE_MODULE_EVENT_BASE=//p')
fi

echo "== start server =="
HOME="$RUN/home" AIMEE_NO_CACHE=1 \
AIMEE_MODULE_BUS_SOCKET="$RUN/mb.sock" \
AIMEE_MODULE_POLICY_DIR="$AH/modules.d/server" \
AIMEE_MCP_TOOL_PROFILE=full \
nohup "$SRV" --foreground --log-level=info > "$RUN/server.log" 2>&1 &
echo $! > "$RUN/server.pid"

SOCK="$AH/aimee-http.sock"
i=0
while [ $i -lt 60 ]; do [ -S "$SOCK" ] && break; sleep 0.5; i=$((i+1)); done
if [ -S "$SOCK" ]; then pass "server /v1 socket is up"; else
  fail "server /v1 socket never appeared"; tail -20 "$RUN/server.log"; exit 1
fi

api() { curl -s --unix-socket "$SOCK" "http://localhost$1"; }
apipost() { curl -s -X POST --unix-socket "$SOCK" -H 'Content-Type: application/json' -d "$2" "http://localhost$1"; }

echo "== start the plugin module instance =="
ln -sf "$MOD" "$RUN/aimee-module-mcp-live"
HOME="$RUN/home" \
AIMEE_MODULE_PRINCIPAL_REF="$REF" \
AIMEE_MODULE_EVENT_BASE="$BASE" \
AIMEE_MCP_PLUGIN_ARGV="[\"python3\",\"$RUN/plugin.py\"]" \
AIMEE_MCP_PLUGIN_PERMISSION=write \
nohup "$RUN/aimee-module-mcp-live" "$RUN/mb.sock" > "$RUN/module.log" 2>&1 &
echo $! > "$RUN/module.pid"

# The daemon refreshes the registry on a TTL; give admission + attach time.
i=0
while [ $i -lt 40 ]; do
  api /v1/dashboard/metrics 2>/dev/null | grep -q '"state":"active"' && break
  sleep 1; i=$((i+1))
done

echo "== operator surface =="
METRICS=$(api /v1/dashboard/metrics)
echo "$METRICS" | grep -q '"plugins"'; check $? "metrics carries a plugins array"
echo "$METRICS" | grep -q '"state":"active"'; check $? "the instance reports state=active"
echo "$METRICS" | grep -q '"group":"live"'; check $? "it reports its command group"

echo "== the command list =="
MAN=$(api /v1/cli/manifest)
echo "$MAN" | grep -q '"live.live_echo"'; check $? "manifest advertises live.live_echo (name folded)"
echo "$MAN" | grep -q '"/v1/commands/live.live_echo"'; check $? "manifest advertises the FULL invoke path"

# The server runs with AIMEE_MCP_TOOL_PROFILE=full here on purpose. Plugin tools
# are registered MCPDiscoverable by design -- mcp_tool_profile.c records that the
# prominent list is a per-session tax on every client, and ~15 plugin modules
# would grow it without bound -- so the default "core" profile correctly filters
# them out and they are reached through find_tools/describe_tool/call_tool. The
# full profile is what proves the group tool is actually BUILT and appended.
echo "== MCP tools/list (full profile) =="
TL=$(api /v1/mcp/tools_list)
echo "$TL" | grep -q '"name":"live"'; check $? "tools/list carries the plugin's group tool"
echo "$TL" | grep -q '"generation"'; check $? "tools_list reports the registry generation"

# HTTP is only the INGRESS to the daemon. The hop that matters is the next one:
# rh_command_invoke -> registry lookup -> plugin_command_invoke ->
# obs_bus_module_call(...) over the EVENT BUS to the Go module, which then
# speaks stdio to the plugin. Nothing between the daemon and the module is HTTP.
echo "== dispatch: HTTP ingress -> event bus -> plugin =="
RES=$(apipost /v1/commands/live.live_echo '{"hello":"world"}')
echo "$RES" | grep -q '"status":"ok"'; check $? "POST /v1/commands/... returned ok"
echo "$RES" | grep -q 'live-plugin'; check $? "the call crossed the event bus and reached the real plugin"
echo "$RES" | grep -q 'world'; check $? "arguments reached the plugin"

echo "== a command that does not exist =="
NOPE=$(apipost /v1/commands/live.nope '{}')
echo "$NOPE" | grep -qi 'no such command'; check $? "an unknown command 404s rather than dispatching"

# --- PARITY: the aimee.yaml path this would retire, on the same server ---
#
# The retirement question is not "does the plugin path work" but "does it do what
# the path it replaces does". So the SAME server also boots a config-declared
# mcp_clients entry, and both are exercised side by side.
echo "== a SECOND instance on the same daemon =="
[ "$REF2" != "$REF" ] && pass "second instance got a distinct principal_ref ($REF vs $REF2)" \
  || fail "principal_ref collision"
[ "$BASE2" != "$BASE" ] && pass "second instance got distinct event kinds ($BASE vs $BASE2)" \
  || fail "event kind collision"
ln -sf "$MOD" "$RUN/aimee-module-mcp-live2"
HOME="$RUN/home" \
AIMEE_MODULE_PRINCIPAL_REF="$REF2" AIMEE_MODULE_EVENT_BASE="$BASE2" \
AIMEE_MCP_PLUGIN_ARGV="[\"python3\",\"$RUN/plugin2.py\"]" \
AIMEE_MCP_PLUGIN_PERMISSION=write \
nohup "$RUN/aimee-module-mcp-live2" "$RUN/mb.sock" > "$RUN/module2.log" 2>&1 &

echo "== a PLUGGY plugin on the same daemon =="
if [ -n "$PGARGV" ]; then
  ln -sf "$MOD" "$RUN/aimee-module-mcp-livepg"
  HOME="$RUN/home" PYTHONPATH="$RUN/pluggylib:$RUN/fixtures" \
  AIMEE_MODULE_PRINCIPAL_REF="$REF3" AIMEE_MODULE_EVENT_BASE="$BASE3" \
  AIMEE_MCP_PLUGIN_ARGV="$PGARGV" \
  AIMEE_MCP_PLUGIN_PERMISSION=write \
  nohup "$RUN/aimee-module-mcp-livepg" "$RUN/mb.sock" > "$RUN/module3.log" 2>&1 &
else
  echo "  (pluggy not shipped; skipping)"
fi

i=0
while [ $i -lt 40 ]; do
  api /v1/cli/manifest 2>/dev/null | grep -q '"live2.live_two"' && break
  sleep 1; i=$((i+1))
done
MAN2=$(api /v1/cli/manifest)
echo "$MAN2" | grep -q '"live.live_echo"'; check $? "instance 1 still advertised alongside instance 2"
if echo "$MAN2" | grep -q '"live2.live_two"'; then
  pass "instance 2 advertised under its own group"
else
  fail "instance 2 advertised under its own group"
  echo "  --- module2.log ---"; tail -5 "$RUN/module2.log" | sed 's/^/  /'
fi
R2=$(apipost /v1/commands/live2.live_two '{}')
echo "$R2" | grep -q 'live-plugin-2'; check $? "instance 2 answered from ITS plugin, not instance 1's"
if [ -d "$RUN/pluggylib" ]; then
  if echo "$MAN2" | grep -q '"livepg.greet"'; then
    pass "the pluggy instance advertised its hook as a command"
  else
    fail "the pluggy instance advertised its hook as a command"
    echo "  --- module3.log ---"; tail -5 "$RUN/module3.log" | sed 's/^/  /'
  fi
  PGRES=$(apipost /v1/commands/livepg.greet '{"name":"ada"}')
  echo "$PGRES" | grep -q 'hello ada'; check $? "a pluggy hook crossed the event bus to the real plugin"
fi

echo "== parity: the aimee.yaml mcp_clients path =="
grep -q 'mcp_clients' "$AH/aimee.yaml" && pass "config declares an mcp_clients entry" || fail "no mcp_clients entry"
CFGTOOLS=$(api /v1/mcp/tools_list)
if echo "$CFGTOOLS" | grep -q 'cfgplugin:'; then
  pass "config-declared client's tools are namespaced client:tool"
else
  fail "config-declared client's tools are absent from tools/list"
  echo "  --- mcp-registry lines from the server log ---"
  grep -i "mcp" "$RUN/server.log" | tail -10 | sed 's/^/  /'
fi

echo "== withdrawal: kill the plugin, keep the module =="
GEN_BEFORE=$(echo "$TL" | sed -n 's/.*"generation":\([0-9]*\).*/\1/p')
pkill -f "$RUN/plugin.py" 2>/dev/null
sleep 1
i=0
while [ $i -lt 40 ]; do
  api /v1/cli/manifest 2>/dev/null | grep -q '"live.live_echo"' || break
  sleep 1; i=$((i+1))
done
api /v1/cli/manifest | grep -q '"live.live_echo"' && fail "a dead plugin kept its command" || pass "a dead plugin's command was withdrawn from the manifest"
GEN_AFTER=$(api /v1/mcp/tools_list | sed -n 's/.*"generation":\([0-9]*\).*/\1/p')
[ -n "$GEN_BEFORE" ] && [ -n "$GEN_AFTER" ] && [ "$GEN_BEFORE" != "$GEN_AFTER" ] \
  && pass "the registry generation moved ($GEN_BEFORE -> $GEN_AFTER), so listChanged fires" \
  || fail "the generation did not move ($GEN_BEFORE -> $GEN_AFTER)"
api /v1/dashboard/metrics | grep -q '"plugins"'; check $? "the module still answers after its plugin died"

echo
if [ "$FAIL" -eq 0 ]; then echo "LIVE VALIDATION PASSED"; else echo "LIVE VALIDATION FAILED ($FAIL)"; fi
exit "$FAIL"
