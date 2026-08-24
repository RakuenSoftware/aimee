#!/bin/bash
# Prove the memory module's RERANK stage is exercised by a live chat turn.
#
# ingress_preinject_build asks for a confidence tier over event 5893 and, if the
# answer does not come, DELETES the envelope it just assembled:
#
#     LOG_WARN("memory", "rerank confidence unavailable; omitting pre-injection envelope");
#     return NULL;
#
# So the envelope's presence in the upstream prompt is the observable: it can
# only be there if RERANK answered. Stopping the module must remove it and log
# that warning; restarting must bring it back.
#
# Requires the capture proxy (start-logging-proxy.sh) so the prompt is read
# rather than inferred. Run AS ROOT in the container.
set -u
Q="Where does the deployment runbook live and who owns it?"

envelope_present() {
  : > /root/proxy-capture.jsonl
  curl -s -m 300 --unix-socket /root/aimee-http.sock -H 'content-type: application/json' \
    -X POST --data "{\"model\":\"qwen\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"$Q\"}]}" \
    http://localhost/v1/messages >/dev/null
  sleep 1
  python3 - <<'PY'
import json
try:
    line = [l for l in open('/root/proxy-capture.jsonl') if l.strip()][-1]
except (OSError, IndexError):
    print("NO-CAPTURE"); raise SystemExit
body = json.loads(json.loads(line)["body"])
text = "\n".join(
    (m.get("content") if isinstance(m.get("content"), str) else json.dumps(m.get("content")))
    for m in body.get("messages", []))
print("PRESENT" if "recommended (memory previews)" in text else "ABSENT")
PY
}

warns() { grep -ac "rerank confidence unavailable" /root/server.log; }

echo "=== module RUNNING ==="
echo "  instances: $(pgrep -cf 'aimee-module-memory /root/server-module-bus.sock')"
w0="$(warns)"
echo "  envelope: $(envelope_present)   (warnings so far: $w0)"

echo
echo "=== module STOPPED ==="
pkill -f "aimee-module-memory /root/server-module-bus.sock" 2>/dev/null
sleep 2
echo "  instances: $(pgrep -cf 'aimee-module-memory /root/server-module-bus.sock')"
echo "  envelope: $(envelope_present)"
w1="$(warns)"
echo "  new 'rerank confidence unavailable' warnings: $(( w1 - w0 ))"

echo
echo "=== module RESTARTED ==="
bash /root/install-memory-module-server.sh >/dev/null 2>&1
sleep 2
echo "  instances: $(pgrep -cf 'aimee-module-memory /root/server-module-bus.sock')"
echo "  envelope: $(envelope_present)"

echo
echo "PRESENT / ABSENT / PRESENT with a warning in the middle means the envelope"
echo "depends on the module answering RERANK -- i.e. the stage is exercised live."
