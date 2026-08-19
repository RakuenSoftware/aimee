#!/bin/bash
# Exploratory pass over the SERVED surface, run inside a disposable container
# against a real aimee-server through the recording proxy.
#
# The differential test proves a spec matches its marshaller. It cannot tell you
# whether the server ACCEPTS what the pair agree on, whether a served-only
# command reaches a handler, or whether an argument shape nobody sampled falls
# over. That is what this is for: drive real commands, record what went on the
# wire, and report what the server said back.
export HOME=/root
export AIMEE_HOME=/root/.config/aimee
export AIMEE_SERVER_URL=http://127.0.0.1:18898
export AIMEE_SERVER_TOKEN=ct-e2e-bearer
A=/opt/aimee/bin/aimee

pkill -f ct-proxy.py 2>/dev/null; sleep 1
rm -f /tmp/wire.jsonl
setsid python3 /tmp/ct-proxy.py > /tmp/proxy.log 2>&1 < /dev/null &
sleep 2
curl -fsS -m 5 -H "Authorization: Bearer ct-e2e-bearer" \
     http://127.0.0.1:18898/v1/health > /dev/null || { echo "PROXY DOWN"; exit 1; }

run() {  # run <label> <args...>
    local label=$1; shift
    local out rc
    out=$("$A" "$@" 2>&1); rc=$?
    printf '%s\t%s\t%s\n' "$rc" "$label" "$(printf '%s' "$out" | head -c 160 | tr '\n' ' ')"
}

echo "=== exploratory sweep (rc, label, first line of output)"

# Newly served this branch, exercising each new vocabulary item.
run "cwd/index.find"        index find marshal_request
run "cwd/index.structure"   index structure src/cli_argspec.c
run "cwd/memory.list"       memory list --limit 3
run "cwd/memory.search"     memory search alpha beta
run "cwd/kb.search"         kb search "thin client"
run "cwd/worktree.gc"       worktree gc --days 9999
run "session/wm.list"       wm list
run "session/wm.get"        wm get somekey
run "session/session.attach" session attach --surface web
run "cascade/memory.recall" memory recall --query "what changed"
run "cascade/mem.recall2"   memory recall --task t --query q
run "arity/delegate.log"    delegate log
run "arity/delegate.log!"   delegate log 42
run "clamp/insights"        insights --days 9999
run "int64/memory.get"      memory get 4294967297
run "skill/skill.list"      skill list
run "skill/skill.eval"      skill eval somename
run "alias/use"             use ollama
run "alias/presence"        presence --owner ada
run "help/verify"           help verify

echo
echo "=== what reached the wire"
python3 - <<'PY'
import json, collections
seen = collections.OrderedDict()
try:
    for line in open("/tmp/wire.jsonl"):
        b = json.loads(line).get("body")
        if isinstance(b, dict) and "method" in b:
            seen.setdefault(b["method"], b)
except FileNotFoundError:
    print("nothing recorded")
    raise SystemExit
for m, b in seen.items():
    keep = {k: v for k, v in b.items() if k != "protocol_version"}
    if isinstance(keep.get("cwd"), str):
        keep["cwd"] = "<cwd>"
    print(f"  {json.dumps(keep)}")
print(f"\n{len(seen)} distinct methods reached the server")
PY
