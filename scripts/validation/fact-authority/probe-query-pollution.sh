#!/bin/bash
# Does the persona block, prepended before the memory stage runs, destroy recall?
#
# aimee_ir_apply_request_stages() inserts the persona onto the first user message
# BEFORE running the stage list, and ir_stage_memory then takes its query from
# aimee_ir_last_user_text() -- the whole message, persona included, up to
# IR_MEMORY_QUERY_MAX (16384). So the "query" put to recall is thousands of
# characters of persona with the user's actual question at the end.
#
# This asks the kb the same question twice: once as the user asked it, once as
# the stage actually asks it (the captured upstream user message).
# Run AS ROOT in the container.
set -u
B="$(cat /root/kb-bearer.txt)"
P="$(cat /root/proj/.git/aimee-project-id)"

rows_for() {  # $1 = query, via a JSON file so quoting cannot corrupt it
  python3 - "$1" "$P" <<'PY' > /tmp/q.json
import json, sys
json.dump({"query": sys.argv[1], "scope_context": True,
           "workspace": sys.argv[2], "project": sys.argv[2], "limit": 5}, open('/tmp/q.json','w'))
PY
  curl -s -m 60 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
       -X POST --data @/tmp/q.json \
       http://127.0.0.1:8741/v1/actions/memory.diagnose_scoped \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(len(d.get("rows") or []))'
}

CLEAN="Where does the deployment runbook live and who owns it?"
echo "clean question                 -> rows=$(rows_for "$CLEAN")"

POLLUTED="$(python3 - <<'PY'
import json
try:
    line = [l for l in open('/root/proxy-capture.jsonl') if l.strip()][-1]
except (OSError, IndexError):
    print(""); raise SystemExit
body = json.loads(json.loads(line)["body"])
for m in body.get("messages", []):
    if m.get("role") == "user":
        c = m.get("content")
        print(c if isinstance(c, str) else json.dumps(c))
        break
PY
)"
if [ -z "$POLLUTED" ]; then
  echo "no captured user message; run show-upstream-prompt.sh first" >&2
  exit 1
fi
echo "as the stage asks it (${#POLLUTED} chars) -> rows=$(rows_for "$POLLUTED")"
echo
echo "If the second is 0 and the first is not, the persona prepend is what empties"
echo "the envelope: the memory query is the persona, not the user's question."
