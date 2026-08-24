#!/bin/bash
# Runs INSIDE CT 9010.
#
# The claim under test: the thin client's behaviour is decided by what the
# SERVER serves, not by what the client was compiled with.
export HOME=/root
export AIMEE_HOME=/root/.config/aimee
export AIMEE_SERVER_URL=http://127.0.0.1:18898
export AIMEE_SERVER_TOKEN=ct-e2e-bearer

pkill -f ct-proxy.py 2>/dev/null
sleep 1
rm -f /tmp/wire.jsonl
setsid python3 /tmp/ct-proxy.py > /tmp/proxy.log 2>&1 < /dev/null &
sleep 2

if ! curl -fsS -m 5 -H "Authorization: Bearer ct-e2e-bearer" \
        http://127.0.0.1:18898/v1/health > /dev/null 2>&1; then
    echo "PROXY NOT UP"; cat /tmp/proxy.log; exit 1
fi
echo "proxy up"

# The id that separates atoi from atoll: above 2^31, far below the 2^53 point
# where a JSON double stops being able to carry an integer exactly.
MID=4294967296
# And the ceiling itself, recorded rather than asserted.
CEIL=9007199254740993

/opt/aimee/bin/aimee memory delete "$MID"                 > /tmp/c1.out 2>&1
/opt/aimee/bin/aimee memory get "$MID" --as_of 2026-01-01 > /tmp/c2.out 2>&1
/opt/aimee/bin/aimee insights --days 9999                 > /tmp/c3.out 2>&1
/opt/aimee/bin/aimee insights                             > /tmp/c4.out 2>&1
/opt/aimee/bin/aimee memory delete "$CEIL"                > /tmp/c5.out 2>&1
/opt/aimee/bin/aimee presence --owner ada                 > /tmp/c6.out 2>&1

echo "--- what the client actually sent:"
python3 - "$MID" "$CEIL" <<'PY'
import json, sys
mid, ceil = int(sys.argv[1]), int(sys.argv[2])
recs = []
try:
    for line in open("/tmp/wire.jsonl"):
        b = json.loads(line).get("body")
        if isinstance(b, dict) and "method" in b:
            recs.append(b)
except FileNotFoundError:
    print("NO WIRE RECORD"); raise SystemExit(1)

for b in recs:
    print("  " + json.dumps({k: v for k, v in b.items() if k != "protocol_version"}))

by = {}
for b in recs:
    by.setdefault(b["method"], []).append(b)

print("\n--- verdict:")
fail = 0
def check(cond, msg):
    global fail
    print(f"  {'PASS' if cond else 'FAIL'} {msg}")
    fail += not cond

dels = by.get("memory.delete", [])
check(bool(dels) and dels[0].get("id") == mid,
      f"memory.delete carries {mid} (> 2^31) intact: {dels[0].get('id') if dels else None!r}")

g = by.get("memory.get", [{}])[0]
check(g.get("id") == mid, f"memory.get carries {mid} intact: {g.get('id')!r}")
check(g.get("as_of") == "2026-01-01", f"memory.get honours --as_of: {g.get('as_of')!r}")

ov = by.get("insights.overview", [])
check(len(ov) >= 2 and ov[0].get("days") == 365, "insights --days 9999 clamps to 365")
check(len(ov) >= 2 and ov[1].get("days") == 30, "insights with no flag defaults to 30")

pres = by.get("session.presence", [])
check(bool(pres) and pres[0].get("owner") == "ada",
      f"session.presence sends owner: {pres[0].get('owner') if pres else None!r}")

# Recorded, not asserted: the JSON-number ceiling both sides share.
if len(dels) >= 2:
    got = dels[1].get("id")
    print(f"\n  NOTE  id {ceil} (2^53+1) arrives as {got!r} — a JSON number is a "
          f"double, so this is the ceiling for ANY integer field, independent of "
          f"which C parse the spec names.")

print("\nRESULT:", "all wire assertions passed" if not fail else f"{fail} failed")
raise SystemExit(1 if fail else 0)
PY
