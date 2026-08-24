#!/bin/bash
# Drive one turn through the capture proxy and print the prompt that actually
# went upstream, so the presence of the pre-injection envelope is read rather
# than inferred from token counts.
# Run AS ROOT in the container.
set -u
Q="${1:-Where does the deployment runbook live and who owns it?}"
: > /root/proxy-capture.jsonl

curl -s -m 300 --unix-socket /root/aimee-http.sock -H 'content-type: application/json' \
  -X POST --data "{\"model\":\"qwen\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"$Q\"}]}" \
  http://localhost/v1/messages >/dev/null
sleep 1

python3 - <<'PY'
import json
try:
    lines = [l for l in open('/root/proxy-capture.jsonl') if l.strip()]
except OSError:
    print('no capture file'); raise SystemExit(1)
if not lines:
    print('NOTHING CAPTURED: the server did not reach the proxy'); raise SystemExit(1)
body = json.loads(json.loads(lines[-1])['body'])
msgs = body.get('messages', [])
print(f'messages: {len(msgs)}')
whole = []
for m in msgs:
    c = m.get('content')
    c = c if isinstance(c, str) else json.dumps(c)
    whole.append(c)
    print(f"-- {m.get('role')}  {len(c)} chars")
    print('   ' + c[:400].replace('\n', ' | '))
text = '\n'.join(whole)
print()
for marker in ('Known facts', 'recommended (memory previews)', 'aimee-context',
               'confidence', 'historical_fact', 'deployment runbook'):
    print(f'  {marker!r:36} present={marker.lower() in text.lower()}')
PY
