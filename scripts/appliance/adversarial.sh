#!/bin/bash
# Adversarial exploratory pass: inputs nobody would sample, driven against a
# real server. The differential test compares two implementations on inputs it
# invented; this asks what the pair actually DO with inputs an operator might
# type by accident.
export HOME=/root
export AIMEE_HOME=/root/.config/aimee
export AIMEE_SERVER_URL=http://127.0.0.1:18898
export AIMEE_SERVER_TOKEN=ct-e2e-bearer
A=/opt/aimee/bin/aimee

pkill -f ct-proxy.py 2>/dev/null; sleep 1
rm -f /tmp/wire.jsonl
setsid python3 /tmp/ct-proxy.py > /tmp/proxy.log 2>&1 < /dev/null &
sleep 2

run() { local l=$1; shift; local o rc; o=$("$A" "$@" 2>&1); rc=$?
        printf '%s\t%s\t%s\n' "$rc" "$l" "$(printf '%s' "$o" | head -c 90 | tr '\n' ' ')"; }

echo "=== adversarial (rc, label, output)"
run "empty-positional"      index find ""
run "flag-shaped-positional" index find --json
run "bare-dashdash"         index find --
run "leading-dash-value"    memory get -5
run "huge-number"           memory get 99999999999999999999
run "negative-clamp"        insights --days -100
run "float-days"            insights --days 3.9
run "garbage-days"          insights --days abc
run "unicode"               index find "héllo→wörld"
run "very-long"             index find "$(python3 -c 'print("x"*4000)')"
run "quotes"                index find 'a"b\c'
run "newline-in-arg"        kb search "$(printf 'a\nb')"
run "many-positionals"      memory search a b c d e f g h
run "flag-no-value"         memory list --limit
run "repeated-flag"         memory list --limit 1 --limit 2
run "equals-form"           memory list --limit=5

echo
echo "=== session precedence with AIMEE_SESSION_ID set"
AIMEE_SESSION_ID=from-env run "env-only"       wm list
AIMEE_SESSION_ID=from-env run "flag-beats-env" wm list --session from-flag

echo
echo "=== bodies"
python3 - <<'PY'
import json
n = 0
for line in open("/tmp/wire.jsonl"):
    b = json.loads(line).get("body")
    if not isinstance(b, dict) or "method" not in b:
        continue
    keep = {k: v for k, v in b.items() if k != "protocol_version"}
    if isinstance(keep.get("cwd"), str):
        keep["cwd"] = "<cwd>"
    s = json.dumps(keep)
    print("  " + (s[:150] + "..." if len(s) > 150 else s))
    n += 1
print(f"\n{n} requests")
PY
