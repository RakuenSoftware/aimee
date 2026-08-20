#!/bin/bash
# Exploratory pass over the SERVED surface, against a real aimee-server through
# the recording proxy.
#
# The differential test proves a spec matches its marshaller. It cannot tell you
# whether the server ACCEPTS what the pair agree on. That is what this is for.
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

run() { local l=$1; shift; local o rc; o=$("$A" "$@" 2>&1); rc=$?
        printf '%s\t%s\t%s\n' "$rc" "$l" "$(printf '%s' "$o" | head -c 110 | tr '\n' ' ')"; }

echo "=== exploratory sweep (rc, label, output)"
# cwd family
run "cwd/index.find"         index find marshal_request
run "cwd/memory.list"        memory list --limit 3
run "cwd/memory.search"      memory search alpha beta
run "cwd/kb.search"          kb search "thin client"
run "cwd/worktree.gc"        worktree gc --days 9999
run "cwd/init.run"           init --anything
# arity gates: one positional vs several
run "arity/structure-1"      index structure src/cli_argspec.c
run "arity/structure-2"      index structure someproject src/cli_argspec.c
run "arity/blast-1"          index blast-radius src/cli_argspec.c
run "arity/hybrid-1"         index hybrid "one query"
run "arity/hybrid-2"         index hybrid alpha beta
run "arity/investigate-2"    index investigate alpha beta
# session cascade
run "session/wm.list"        wm list
run "session/session.attach" session attach --surface web
# literal-equality gates
run "equals/skill.lint-all"  skill lint --all
run "equals/skill.lint-name" skill lint someskill
run "equals/skill.patch"     skill patch s old new
run "equals/skill.patch-all" skill patch s old new --all
run "equals/skill.pin"       skill pin someskill
# cascades, clamps, widths, arity refusal
run "cascade/memory.recall"  memory recall --query "what changed"
run "arity/delegate.log"     delegate log
run "arity/delegate.log!"    delegate log 42
run "clamp/insights"         insights --days 9999
run "int64/memory.get"       memory get 4294967297
run "alias/use"              use ollama
run "alias/presence"         presence --owner ada

echo
echo "=== what reached the wire"
python3 - <<'PY'
import json, collections
seen = collections.OrderedDict()
try:
    for line in open("/tmp/wire.jsonl"):
        b = json.loads(line).get("body")
        if isinstance(b, dict) and "method" in b:
            seen.setdefault((b["method"], json.dumps(sorted(b))), b)
except FileNotFoundError:
    print("nothing recorded"); raise SystemExit
for _, b in seen.items():
    keep = {k: v for k, v in b.items() if k != "protocol_version"}
    if isinstance(keep.get("cwd"), str):
        keep["cwd"] = "<cwd>"
    print("  " + json.dumps(keep))
print(f"\n{len({m for m, _ in seen})} distinct methods, {len(seen)} distinct shapes")
PY
